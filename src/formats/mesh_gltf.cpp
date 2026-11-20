#include "formats/mesh_gltf.h"

#include "formats/json_value.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <format>
#include <optional>
#include <span>
#include <vector>

namespace luciole::formats {
  namespace {
    using Status = std::expected<void, std::string>;

    constexpr std::size_t kTriangleMode = 4;
    constexpr std::size_t kNoIndex = std::numeric_limits<std::size_t>::max();

    // The object at `index` in the top-level array `name`, whose items are `items`.
    std::expected<const JsonValue*, std::string>
    rootItem(std::span<const JsonValue> items, std::string_view name, std::size_t index) {
      if (index >= items.size() || items[index].type != JsonType::Object) {
        return std::unexpected(std::format("the glTF {} entry {} does not exist", name, index));
      }
      return &items[index];
    }

    std::expected<Transform, std::string> localTransform(const JsonValue& node) {
      if (const JsonValue* matrix = node.find("matrix"); matrix != nullptr) {
        const auto values = numbers<16>(matrix);
        return values ? std::expected<Transform, std::string>(*values)
                      : std::unexpected("a glTF node matrix does not hold 16 numbers");
      }
      const auto translation = numbersMember<3>(node, "translation", {0, 0, 0});
      const auto rotation = numbersMember<4>(node, "rotation", {0, 0, 0, 1});
      const auto scale = numbersMember<3>(node, "scale", {1, 1, 1});
      if (!translation || !rotation || !scale) {
        return std::unexpected("a glTF node translation, rotation or scale has the wrong size");
      }
      return composeTransform(*translation, *rotation, *scale);
    }

    // An accessor's validated layout; `bytes` starts at its first element.
    struct Accessor {
      std::string_view bytes;
      std::size_t count = 0;
      std::size_t componentType = 0;
      std::size_t stride = 0;
      bool normalized = false;
      bool zero = false; // no buffer view: every value is zero
    };

    // Bytes per component for the types 5120 to 5126; 0 for an unknown type.
    std::size_t componentSize(std::size_t type) {
      constexpr std::array<std::size_t, 7> kSizes = {1, 1, 2, 2, 0, 4, 4};
      return type >= 5120 && type <= 5126 ? kSizes.at(type - 5120) : 0;
    }

    // One component as a double; the accessor was validated, so the read is inside its bytes.
    double component(const Accessor& accessor, std::size_t element, std::size_t index) {
      if (accessor.zero) {
        return 0.0;
      }
      const std::size_t at = (element * accessor.stride) + (index * componentSize(accessor.componentType));
      const auto read = [&]<typename T>(T unit) {
        const double value = mesh_io::readScalar<T>(accessor.bytes, at).value();
        return accessor.normalized ? std::max(value / static_cast<double>(unit), -1.0) : value;
      };
      switch (accessor.componentType) {
      case 5120:
        return read(std::numeric_limits<std::int8_t>::max());
      case 5121:
        return read(std::numeric_limits<std::uint8_t>::max());
      case 5122:
        return read(std::numeric_limits<std::int16_t>::max());
      case 5123:
        return read(std::numeric_limits<std::uint16_t>::max());
      case 5125:
        return read(std::numeric_limits<std::uint32_t>::max());
      default:
        return mesh_io::readScalar<float>(accessor.bytes, at).value();
      }
    }

    // A triangle-list primitive with its accessors validated.
    struct Draw {
      Accessor positions;
      std::optional<Accessor> indices;
      std::optional<Accessor> normals;
    };

    class GltfReader {
    public:
      GltfReader(
          const JsonValue& root, const gltf_io::Container& container, const BufferLoader& loader,
          const MeshLimits& limits
      )
          : m_root(root), m_nodes(arrayItems(root.find("nodes"))), m_meshes(arrayItems(root.find("meshes"))),
            m_accessors(arrayItems(root.find("accessors"))), m_views(arrayItems(root.find("bufferViews"))),
            m_bufferItems(arrayItems(root.find("buffers"))), m_binary(container.binary), m_loader(loader),
            m_limits(limits) {}

      Status read();
      Mesh& mesh() { return m_mesh; }

    private:
      std::expected<std::string_view, std::string> buffer(std::size_t index);
      // `components` is 1 for a SCALAR accessor or 3 for a VEC3 one.
      std::expected<Accessor, std::string> accessor(const JsonValue* indexMember, std::size_t components);
      // The primitive's draw, or nothing when it is not drawn; records the parts it skips.
      std::expected<std::optional<Draw>, std::string> resolvePrimitive(const JsonValue& primitive);
      // The draws of a mesh, resolved on its first use.
      std::expected<const std::vector<Draw>*, std::string> meshDraws(const JsonValue* indexMember);
      Status drawPrimitive(const Draw& draw, const PointMapper& place);
      Status addMesh(const JsonValue* indexMember, const Transform& world);
      Status addNode(std::size_t index, const Transform& parent, std::size_t depth);

      const JsonValue& m_root;
      std::span<const JsonValue> m_nodes;
      std::span<const JsonValue> m_meshes;
      std::span<const JsonValue> m_accessors;
      std::span<const JsonValue> m_views;
      std::span<const JsonValue> m_bufferItems;
      std::optional<std::string_view> m_binary;
      const BufferLoader& m_loader;
      MeshLimits m_limits;
      std::vector<std::optional<std::string>> m_owned; // decoded or loaded buffers, one slot per buffer
      std::vector<std::optional<std::string_view>> m_buffers;
      std::vector<std::optional<std::vector<Draw>>> m_meshDraws; // one slot per mesh
      Mesh m_mesh;
      std::vector<bool> m_visited; // one flag per node
      bool m_allNormals = true;
      bool m_full = false;
    };

    std::expected<std::string_view, std::string> GltfReader::buffer(std::size_t index) {
      if (index < m_buffers.size() && m_buffers[index]) {
        return *m_buffers[index];
      }
      const auto object = rootItem(m_bufferItems, "buffers", index);
      const auto byteLength = object ? wholeMember(**object, "byteLength") : std::unexpected(object.error());
      if (!byteLength) {
        return std::unexpected(byteLength.error());
      }
      const JsonValue* uriMember = (*object)->find("uri");
      const std::string_view uri = uriMember == nullptr ? std::string_view{} : std::string_view(uriMember->text);
      std::optional<std::string>& owned = m_owned.at(index);
      if (uriMember == nullptr && (index != 0 || !m_binary)) {
        return std::unexpected("a glTF buffer has no data");
      }
      if (uri.starts_with("data:")) {
        const std::size_t comma = uri.find(',');
        if (comma != std::string_view::npos && uri.substr(0, comma).ends_with(";base64")) {
          owned = gltf_io::decodeBase64(uri.substr(comma + 1));
        }
        if (!owned) {
          return std::unexpected("a glTF data URI is not valid base64");
        }
      } else if (uriMember != nullptr) {
        const std::optional<std::string> path = gltf_io::bufferPath(uri);
        if (!path) {
          return std::unexpected("a glTF buffer URI is absolute, has a scheme or leaves its folder");
        }
        owned = m_loader ? m_loader(*path) : std::nullopt;
        if (!owned) {
          return std::unexpected("a glTF buffer file could not be read");
        }
      }
      const std::string_view data = owned ? std::string_view(*owned) : *m_binary;
      if (data.size() < *byteLength) {
        return std::unexpected("a glTF buffer is shorter than its byteLength");
      }
      m_buffers.at(index) = data.substr(0, *byteLength);
      return *m_buffers.at(index);
    }

    std::expected<Accessor, std::string> GltfReader::accessor(const JsonValue* indexMember, std::size_t components) {
      const std::size_t index = wholeNumber(indexMember).value_or(kNoIndex);
      const auto object = rootItem(m_accessors, "accessors", index);
      if (!object) {
        return std::unexpected(object.error());
      }
      const JsonValue& value = **object;
      const JsonValue* type = value.find("type");
      const auto componentType = wholeMember(value, "componentType");
      const auto count = wholeMember(value, "count");
      const auto offset = wholeMember(value, "byteOffset", 0);
      const bool typed = type != nullptr && type->text == (components == 1 ? "SCALAR" : "VEC3");
      if (!typed || !componentType || componentSize(*componentType) == 0 || !count || *count == 0 || !offset) {
        return std::unexpected(std::format("the glTF accessor {} has an invalid type, count or offset", index));
      }
      const JsonValue* normalized = value.find("normalized");
      Accessor result{
          .bytes = {},
          .count = *count,
          .componentType = *componentType,
          .normalized = normalized != nullptr && normalized->boolean,
      };
      m_mesh.skipped.sparse = m_mesh.skipped.sparse || value.find("sparse") != nullptr;
      const JsonValue* viewMember = value.find("bufferView");
      result.zero = viewMember == nullptr;
      if (result.zero) {
        return result;
      }
      const std::size_t elementSize = components * componentSize(*componentType);
      const auto view = rootItem(m_views, "bufferViews", wholeNumber(viewMember).value_or(kNoIndex));
      if (!view) {
        return std::unexpected(view.error());
      }
      const auto bufferIndex = wholeMember(**view, "buffer");
      const auto viewOffset = wholeMember(**view, "byteOffset", 0);
      const auto viewLength = wholeMember(**view, "byteLength");
      const auto stride = wholeMember(**view, "byteStride", elementSize);
      if (!bufferIndex || !viewOffset || !viewLength || !stride || *stride < elementSize) {
        return std::unexpected("a glTF buffer view has an invalid buffer, offset, length or stride");
      }
      const auto data = buffer(*bufferIndex);
      if (!data) {
        return std::unexpected(data.error());
      }
      const auto viewEnd = mesh_io::checkedAdd(*viewOffset, *viewLength);
      const auto span = mesh_io::checkedMul(*count - 1, *stride)
                            .and_then([&](std::size_t bytes) { return mesh_io::checkedAdd(bytes, *offset); })
                            .and_then([&](std::size_t bytes) { return mesh_io::checkedAdd(bytes, elementSize); });
      if (!viewEnd || *viewEnd > data->size() || !span || *span > *viewLength) {
        return std::unexpected(std::format("the glTF accessor {} reaches past its buffer view or buffer", index));
      }
      result.stride = *stride;
      result.bytes = data->substr(*viewOffset + *offset, *viewLength - *offset);
      return result;
    }

    std::expected<std::optional<Draw>, std::string> GltfReader::resolvePrimitive(const JsonValue& primitive) {
      const auto mode = wholeMember(primitive, "mode", kTriangleMode);
      if (!mode) {
        return std::unexpected(mode.error());
      }
      const JsonValue* extensions = primitive.find("extensions");
      const JsonValue* attributes = primitive.find("attributes");
      const JsonValue* positionMember = attributes == nullptr ? nullptr : attributes->find("POSITION");
      m_mesh.skipped.morphTargets = m_mesh.skipped.morphTargets || primitive.find("targets") != nullptr;
      m_mesh.skipped.materials = m_mesh.skipped.materials || primitive.find("material") != nullptr;
      if (extensions != nullptr && extensions->find("KHR_draco_mesh_compression") != nullptr) {
        m_mesh.skipped.compressed = true;
        return std::optional<Draw>{};
      }
      m_mesh.skipped.otherModes = m_mesh.skipped.otherModes || *mode != kTriangleMode;
      if (*mode != kTriangleMode || positionMember == nullptr) {
        return std::optional<Draw>{};
      }
      const auto positions = accessor(positionMember, 3);
      if (!positions) {
        return std::unexpected(positions.error());
      }
      std::optional<Accessor> indices;
      if (const JsonValue* indexMember = primitive.find("indices"); indexMember != nullptr) {
        const auto read = accessor(indexMember, 1);
        if (!read || (read->componentType != 5121 && read->componentType != 5123 && read->componentType != 5125)) {
          return std::unexpected(read ? "glTF indices are not unsigned integers" : read.error());
        }
        indices = *read;
      }
      std::optional<Accessor> normals;
      if (const JsonValue* normalMember = attributes->find("NORMAL"); normalMember != nullptr) {
        const auto read = accessor(normalMember, 3);
        if (!read || read->count != positions->count) {
          return std::unexpected(read ? "a glTF NORMAL accessor has another count than POSITION" : read.error());
        }
        normals = *read;
      }
      return Draw{.positions = *positions, .indices = indices, .normals = normals};
    }

    Status GltfReader::drawPrimitive(const Draw& draw, const PointMapper& place) {
      const Accessor& positions = draw.positions;
      const std::optional<Accessor>& indices = draw.indices;
      const std::optional<Accessor>& normals = draw.normals;
      const std::size_t vertexCap =
          std::min<std::size_t>(m_limits.maxVertices, std::numeric_limits<std::uint32_t>::max());
      const std::size_t vertexRoom = vertexCap - std::min(vertexCap, m_mesh.positions.size());
      const std::size_t triangleRoom = m_limits.maxTriangles - std::min(m_limits.maxTriangles, m_mesh.triangleCount());
      const std::size_t triangles = (indices ? indices->count : positions.count) / 3;
      m_full = positions.count > vertexRoom || triangles > triangleRoom;
      m_mesh.truncated = m_mesh.truncated || m_full;
      if (positions.count > vertexRoom) {
        return {};
      }
      const auto base = static_cast<std::uint32_t>(m_mesh.positions.size());
      m_allNormals = m_allNormals && normals.has_value();
      // Bounded by the accessor count, which fits the vertex cap.
      for (std::size_t v = 0; v < positions.count; ++v) {
        const auto at = [&](const Accessor& source, std::size_t index) { return component(source, v, index); };
        m_mesh.positions.push_back(place.point(at(positions, 0), at(positions, 1), at(positions, 2)));
        m_mesh.normals.push_back(normals ? place.normal(at(*normals, 0), at(*normals, 1), at(*normals, 2)) : Vec3{});
      }
      // Bounded by the triangle cap.
      for (std::size_t t = 0; t < std::min(triangles, triangleRoom); ++t) {
        std::array<std::size_t, 3> corner{};
        for (std::size_t k = 0; k < 3; ++k) {
          const std::size_t at = (t * 3) + k;
          corner.at(k) = indices ? static_cast<std::size_t>(component(*indices, at, 0)) : at;
          if (corner.at(k) >= positions.count) {
            return std::unexpected("a glTF index names a vertex that does not exist");
          }
        }
        if (place.mirrors()) {
          std::swap(corner[1], corner[2]);
        }
        for (const std::size_t c : corner) {
          m_mesh.indices.push_back(base + static_cast<std::uint32_t>(c));
        }
      }
      return {};
    }

    std::expected<const std::vector<Draw>*, std::string> GltfReader::meshDraws(const JsonValue* indexMember) {
      const std::size_t index = wholeNumber(indexMember).value_or(kNoIndex);
      const auto mesh = rootItem(m_meshes, "meshes", index);
      if (!mesh) {
        return std::unexpected(mesh.error());
      }
      std::optional<std::vector<Draw>>& draws = m_meshDraws.at(index);
      if (!draws) {
        std::vector<Draw> resolved;
        // Bounded by the primitives listed; each mesh is resolved once.
        for (const JsonValue& primitive : arrayItems((*mesh)->find("primitives"))) {
          const auto draw = resolvePrimitive(primitive);
          if (!draw) {
            return std::unexpected(draw.error());
          }
          if (*draw) {
            resolved.push_back(**draw);
          }
        }
        draws = std::move(resolved);
      }
      return &*draws;
    }

    Status GltfReader::addMesh(const JsonValue* indexMember, const Transform& world) {
      const auto draws = meshDraws(indexMember);
      if (!draws) {
        return std::unexpected(draws.error());
      }
      const PointMapper place(world);
      // Each pass adds at least one vertex or fills the mesh, so the vertex cap bounds the passes of all nodes.
      for (const Draw& draw : **draws) {
        if (m_full) {
          break;
        }
        if (const Status status = drawPrimitive(draw, place); !status) {
          return status;
        }
      }
      return {};
    }

    // Recursion is bounded by kMaxGltfNodeDepth; each node is drawn at most once.
    Status GltfReader::addNode(std::size_t index, const Transform& parent, std::size_t depth) {
      if (index >= m_visited.size() || m_visited[index] || depth > kMaxGltfNodeDepth) {
        return std::unexpected(
            "the glTF scene names a missing node, or its nodes do not form a tree or nest too deeply"
        );
      }
      m_visited[index] = true;
      const JsonValue& node = m_nodes[index];
      const auto local = localTransform(node);
      if (!local) {
        return std::unexpected(local.error());
      }
      const Transform world = multiply(parent, *local);
      m_mesh.skipped.skins = m_mesh.skipped.skins || node.find("skin") != nullptr;
      if (const JsonValue* mesh = node.find("mesh"); mesh != nullptr) {
        if (const Status status = addMesh(mesh, world); !status) {
          return status;
        }
      }
      // Bounded by the children listed.
      for (const JsonValue& child : arrayItems(node.find("children"))) {
        if (const Status status = addNode(wholeNumber(&child).value_or(kNoIndex), world, depth + 1); !status) {
          return status;
        }
      }
      return {};
    }

    // The scene's root nodes, or every node that is no other node's child when there are no scenes. A scene
    // that does not exist, or nodes without scenes of which none is a root, give one root that does not exist,
    // which the walk reports.
    std::vector<std::size_t> rootNodes(const JsonValue& root, std::span<const JsonValue> nodes) {
      std::vector<std::size_t> roots;
      const std::span<const JsonValue> scenes = arrayItems(root.find("scenes"));
      if (!scenes.empty()) {
        const auto scene = rootItem(scenes, "scenes", wholeMember(root, "scene", 0).value_or(kNoIndex));
        for (const JsonValue& item : arrayItems(scene ? (*scene)->find("nodes") : nullptr)) {
          roots.push_back(wholeNumber(&item).value_or(kNoIndex));
        }
        return scene ? roots : std::vector<std::size_t>{kNoIndex};
      }
      const std::size_t nodeCount = nodes.size();
      std::vector<bool> child(nodeCount + 1);
      for (const JsonValue& node : nodes) {
        for (const JsonValue& item : arrayItems(node.find("children"))) {
          const std::size_t index = wholeNumber(&item).value_or(kNoIndex);
          child[std::min(index, nodeCount)] = true; // the walk reports a child that does not exist
        }
      }
      for (std::size_t k = 0; k < nodeCount; ++k) {
        if (!child[k]) {
          roots.push_back(k);
        }
      }
      return roots.empty() && nodeCount > 0 ? std::vector<std::size_t>{kNoIndex} : roots;
    }

    Status GltfReader::read() {
      const JsonValue* asset = m_root.find("asset");
      const JsonValue* version = asset == nullptr ? nullptr : asset->find("version");
      if (version == nullptr || !version->text.starts_with("2.")) {
        return std::unexpected("not a glTF 2.0 file");
      }
      const JsonValue* required = m_root.find("extensionsRequired");
      m_mesh.skipped.extensions = !arrayItems(m_root.find("extensionsUsed")).empty() || !arrayItems(required).empty();
      m_mesh.skipped.skins = !arrayItems(m_root.find("skins")).empty();
      m_mesh.skipped.materials = !arrayItems(m_root.find("materials")).empty();
      m_mesh.skipped.compressed = holdsAnyString(
          required, {"KHR_draco_mesh_compression", "EXT_meshopt_compression", "KHR_meshopt_compression"}
      );
      if (holdsAnyString(required, {"EXT_meshopt_compression", "KHR_meshopt_compression"})) {
        return {}; // the buffers hold compressed data only
      }
      m_owned.resize(m_bufferItems.size());
      m_buffers.resize(m_bufferItems.size());
      m_meshDraws.resize(m_meshes.size());
      m_visited.assign(m_nodes.size(), false);
      for (const std::size_t root : rootNodes(m_root, m_nodes)) {
        if (const Status status = addNode(root, kIdentityTransform, 1); !status) {
          return status;
        }
      }
      if (!m_allNormals) {
        m_mesh.normals.clear();
      }
      return {};
    }
  } // namespace

  Mesh readGltf(std::string_view bytes, const BufferLoader& loadBuffer, const MeshLimits& limits) {
    const auto container = gltf_io::splitContainer(bytes);
    if (!container) {
      return meshError(container.error());
    }
    const auto root = parseJson(container->json);
    if (!root || root->type != JsonType::Object) {
      return meshError(root ? "the glTF JSON is not an object" : "the glTF JSON is malformed: " + root.error());
    }
    GltfReader reader(*root, *container, loadBuffer, limits);
    if (const Status status = reader.read(); !status) {
      return meshError(status.error());
    }
    finishMesh(reader.mesh());
    return std::move(reader.mesh());
  }
} // namespace luciole::formats
