#include "formats/mesh_ply.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <utility>

namespace luciole::formats {
  namespace {
    enum class PlyType : std::uint8_t { Int8, UInt8, Int16, UInt16, Int32, UInt32, Float32, Float64 };
    enum class PlyFormat : std::uint8_t { Ascii, LittleEndian, BigEndian };

    struct Property {
      std::string name;
      PlyType type = PlyType::Float32;    // the value type, or the item type of a list
      PlyType countType = PlyType::UInt8; // the length type of a list
      bool list = false;
    };

    struct Element {
      std::string name;
      std::size_t count = 0;
      std::vector<Property> properties;
    };

    struct Header {
      PlyFormat format = PlyFormat::Ascii;
      std::vector<Element> elements;
      std::size_t bodyOffset = 0;
    };

    using Status = std::expected<void, std::string_view>;
    constexpr std::size_t kNone = static_cast<std::size_t>(-1);
    constexpr std::string_view kEndsEarly = "the PLY file ends early or holds a value that is not a number";

    std::optional<PlyType> typeNamed(std::string_view name) {
      static constexpr std::array<std::pair<std::string_view, PlyType>, 16> kTypes = {{
          {"char", PlyType::Int8},
          {"int8", PlyType::Int8},
          {"uchar", PlyType::UInt8},
          {"uint8", PlyType::UInt8},
          {"short", PlyType::Int16},
          {"int16", PlyType::Int16},
          {"ushort", PlyType::UInt16},
          {"uint16", PlyType::UInt16},
          {"int", PlyType::Int32},
          {"int32", PlyType::Int32},
          {"uint", PlyType::UInt32},
          {"uint32", PlyType::UInt32},
          {"float", PlyType::Float32},
          {"float32", PlyType::Float32},
          {"double", PlyType::Float64},
          {"float64", PlyType::Float64},
      }};
      const auto* found = std::ranges::find(kTypes, name, &std::pair<std::string_view, PlyType>::first);
      return found == kTypes.end() ? std::nullopt : std::optional(found->second);
    }

    std::size_t typeSize(PlyType type) {
      switch (type) {
      case PlyType::Int8:
      case PlyType::UInt8:
        return 1;
      case PlyType::Int16:
      case PlyType::UInt16:
        return 2;
      case PlyType::Int32:
      case PlyType::UInt32:
      case PlyType::Float32:
        return 4;
      case PlyType::Float64:
        return 8;
      }
      return 8;
    }

    Status parseProperty(std::string_view line, std::size_t pos, Header& header) {
      if (header.elements.empty()) {
        return std::unexpected("a PLY property comes before any element");
      }
      Property property;
      std::string_view typeName = mesh_io::nextToken(line, pos);
      if (typeName == "list") {
        const std::optional<PlyType> countType = typeNamed(mesh_io::nextToken(line, pos));
        if (!countType || *countType == PlyType::Float32 || *countType == PlyType::Float64) {
          return std::unexpected("a PLY list has no integer length type");
        }
        property.list = true;
        property.countType = *countType;
        typeName = mesh_io::nextToken(line, pos);
      }
      const std::optional<PlyType> type = typeNamed(typeName);
      property.name = std::string(mesh_io::nextToken(line, pos));
      if (!type || property.name.empty()) {
        return std::unexpected("a PLY property has an unknown type or no name");
      }
      property.type = *type;
      header.elements.back().properties.push_back(std::move(property));
      return {};
    }

    Status parseElement(std::string_view line, std::size_t pos, Header& header) {
      if (header.elements.size() == kMaxPlyElements) {
        return std::unexpected("the PLY header declares too many elements");
      }
      Element element;
      element.name = std::string(mesh_io::nextToken(line, pos));
      const std::string_view count = mesh_io::nextToken(line, pos);
      const auto [ptr, ec] = std::from_chars(count.data(), count.data() + count.size(), element.count);
      if (element.name.empty() || count.empty() || ec != std::errc{} || ptr != count.data() + count.size()) {
        return std::unexpected("a PLY element has no valid count");
      }
      header.elements.push_back(std::move(element));
      return {};
    }

    std::expected<Header, std::string_view> parseHeader(std::string_view bytes) {
      if (!bytes.starts_with("ply\n") && !bytes.starts_with("ply\r\n")) {
        return std::unexpected("not a PLY file");
      }
      Header header;
      bool formatSeen = false;
      std::size_t propertyTotal = 0;
      std::size_t pos = bytes.find('\n') + 1;
      // Each pass consumes one line.
      while (pos < bytes.size()) {
        const std::size_t end = bytes.find('\n', pos);
        if (end == std::string_view::npos) {
          break;
        }
        const std::string_view line = bytes.substr(pos, end - pos);
        pos = end + 1;
        std::size_t at = 0;
        const std::string_view keyword = mesh_io::nextToken(line, at);
        Status status;
        if (keyword == "end_header") {
          if (!formatSeen) {
            return std::unexpected("the PLY header has no format");
          }
          header.bodyOffset = pos;
          return header;
        }
        if (keyword == "format") {
          const std::string_view name = mesh_io::nextToken(line, at);
          formatSeen = name == "ascii" || name == "binary_little_endian" || name == "binary_big_endian";
          header.format = name == "ascii"      ? PlyFormat::Ascii
              : name == "binary_little_endian" ? PlyFormat::LittleEndian
                                               : PlyFormat::BigEndian;
          status = formatSeen && mesh_io::nextToken(line, at) == "1.0"
              ? Status{}
              : Status{std::unexpected("the PLY format is not ascii or binary 1.0")};
        } else if (keyword == "element") {
          status = parseElement(line, at, header);
        } else if (keyword == "property") {
          status = ++propertyTotal > kMaxPlyProperties
              ? Status{std::unexpected("the PLY header declares too many properties")}
              : parseProperty(line, at, header);
        } else if (keyword != "comment" && keyword != "obj_info" && !keyword.empty()) {
          status = std::unexpected("the PLY header has an unknown line");
        }
        if (!status) {
          return std::unexpected(status.error());
        }
      }
      return std::unexpected("the PLY header has no end_header");
    }

    // Reads values in file order from the body; every read is checked against the bytes left.
    class BodyReader {
    public:
      BodyReader(std::string_view bytes, std::size_t offset, PlyFormat format)
          : m_bytes(bytes), m_pos(offset), m_format(format) {}

      std::optional<double> value(PlyType type) {
        if (m_format == PlyFormat::Ascii) {
          return mesh_io::parseNumber(mesh_io::nextToken(m_bytes, m_pos));
        }
        switch (type) {
        case PlyType::Int8:
          return read<std::int8_t>();
        case PlyType::UInt8:
          return read<std::uint8_t>();
        case PlyType::Int16:
          return read<std::int16_t>();
        case PlyType::UInt16:
          return read<std::uint16_t>();
        case PlyType::Int32:
          return read<std::int32_t>();
        case PlyType::UInt32:
          return read<std::uint32_t>();
        case PlyType::Float32:
          return read<float>();
        case PlyType::Float64:
          return read<double>();
        }
        return std::nullopt;
      }

      bool skip(std::size_t count) {
        if (count > remaining()) {
          return false;
        }
        m_pos += count;
        return true;
      }

      std::size_t remaining() const { return m_bytes.size() - m_pos; }
      bool ascii() const { return m_format == PlyFormat::Ascii; }

      // The fewest bytes one value of `type` can take.
      std::size_t minimumSize(PlyType type) const { return ascii() ? 1 : typeSize(type); }

    private:
      template <typename T> std::optional<double> read() {
        const auto order = m_format == PlyFormat::BigEndian ? std::endian::big : std::endian::little;
        const std::optional<T> scalar = mesh_io::readScalar<T>(m_bytes, m_pos, order);
        if (!scalar) {
          return std::nullopt;
        }
        m_pos += sizeof(T);
        return static_cast<double>(*scalar);
      }

      std::string_view m_bytes;
      std::size_t m_pos = 0;
      PlyFormat m_format = PlyFormat::Ascii;
    };

    // A whole number in [0, limit), or nothing.
    std::optional<std::size_t> wholeBelow(double value, std::size_t limit) {
      if (!(value >= 0.0) || value != std::floor(value) || !(value < static_cast<double>(limit))) {
        return std::nullopt;
      }
      return static_cast<std::size_t>(value);
    }

    // Reads one element instance. Scalars land in `scalars` by property position; at most `itemCap` items of the
    // list at `listIndex` land in `items`. Returns that list's full length, or 0 when there is none.
    std::expected<std::size_t, std::string_view> readInstance(
        BodyReader& body, const Element& element, std::span<double> scalars, std::size_t listIndex,
        std::vector<double>& items, std::size_t itemCap
    ) {
      std::size_t chosenLength = 0;
      items.clear();
      for (std::size_t p = 0; p < element.properties.size(); ++p) {
        const Property& property = element.properties[p];
        const std::optional<double> first = body.value(property.list ? property.countType : property.type);
        if (!first) {
          return std::unexpected(kEndsEarly);
        }
        if (!property.list) {
          scalars[p] = *first;
          continue;
        }
        const std::optional<std::size_t> length =
            wholeBelow(*first, (body.remaining() / body.minimumSize(property.type)) + 1);
        if (!length) {
          return std::unexpected("a PLY list has a negative length or is longer than the file");
        }
        // Bounded by the bytes left, checked above.
        for (std::size_t k = 0; k < *length; ++k) {
          const std::optional<double> item = body.value(property.type);
          if (!item) {
            return std::unexpected(kEndsEarly);
          }
          if (p == listIndex && items.size() < itemCap) {
            items.push_back(*item);
          }
        }
        chosenLength = p == listIndex ? *length : chosenLength;
      }
      return chosenLength;
    }

    struct PlyBuild {
      Mesh mesh;
      std::size_t vertexTotal = 0;  // vertices the header declares
      std::size_t vertexStored = 0; // vertices kept under the cap
    };

    std::size_t propertyIndex(const Element& element, std::string_view name, bool list) {
      const auto found = std::ranges::find_if(element.properties, [&](const Property& property) {
        return property.name == name && property.list == list;
      });
      return found == element.properties.end() ? kNone : static_cast<std::size_t>(found - element.properties.begin());
    }

    Status readVertices(BodyReader& body, const Element& element, PlyBuild& build) {
      const std::array<std::size_t, 3> xyz = {
          propertyIndex(element, "x", false), propertyIndex(element, "y", false), propertyIndex(element, "z", false)
      };
      const std::array<std::size_t, 3> nxyz = {
          propertyIndex(element, "nx", false), propertyIndex(element, "ny", false), propertyIndex(element, "nz", false)
      };
      if (std::ranges::contains(xyz, kNone)) {
        return std::unexpected("PLY vertices have no x, y and z");
      }
      const bool normals = !std::ranges::contains(nxyz, kNone);
      std::vector<double> scalars(element.properties.size());
      std::vector<double> items;
      build.mesh.positions.reserve(build.vertexStored);
      // Bounded by the count, which was checked against the bytes left.
      for (std::size_t v = 0; v < element.count; ++v) {
        const auto read = readInstance(body, element, scalars, kNone, items, 0);
        if (!read) {
          return std::unexpected(read.error());
        }
        if (v >= build.vertexStored) {
          continue;
        }
        const auto at = [&](std::size_t index) { return mesh_io::toFloat(scalars[index]); };
        build.mesh.positions.push_back(Vec3{at(xyz[0]), at(xyz[1]), at(xyz[2])});
        if (normals) {
          build.mesh.normals.push_back(Vec3{at(nxyz[0]), at(nxyz[1]), at(nxyz[2])});
        }
      }
      return {};
    }

    Status readFaces(BodyReader& body, const Element& element, PlyBuild& build, const MeshLimits& limits) {
      std::size_t listIndex = propertyIndex(element, "vertex_indices", true);
      listIndex = listIndex == kNone ? propertyIndex(element, "vertex_index", true) : listIndex;
      if (listIndex == kNone) {
        return std::unexpected("PLY faces have no vertex_indices list");
      }
      std::vector<double> scalars(element.properties.size());
      std::vector<double> items;
      std::vector<std::uint32_t> corners;
      Mesh& mesh = build.mesh;
      // Bounded by the count, which was checked against the bytes left.
      for (std::size_t f = 0; f < element.count; ++f) {
        const std::size_t remaining = limits.maxTriangles - std::min(limits.maxTriangles, mesh.triangleCount());
        const std::size_t itemCap = remaining == 0 ? 0 : remaining + 2;
        const auto length = readInstance(body, element, scalars, listIndex, items, itemCap);
        if (!length) {
          return std::unexpected(length.error());
        }
        corners.clear();
        for (const double item : items) {
          const std::optional<std::size_t> index = wholeBelow(item, build.vertexTotal);
          if (!index) {
            return std::unexpected("a PLY face names a vertex that does not exist");
          }
          corners.push_back(static_cast<std::uint32_t>(std::min<std::size_t>(*index, build.vertexStored)));
        }
        if (*length < 3) {
          continue;
        }
        mesh.truncated = mesh.truncated || items.size() < *length || std::ranges::contains(corners, build.vertexStored);
        if (corners.size() < 3 || std::ranges::contains(corners, build.vertexStored)) {
          continue;
        }
        for (std::size_t k = 1; k + 1 < corners.size(); ++k) {
          mesh.indices.insert(mesh.indices.end(), {corners[0], corners[k], corners[k + 1]});
        }
      }
      return {};
    }

    // The fewest bytes one instance of the element can take.
    std::size_t minimumInstanceSize(const BodyReader& body, const Element& element) {
      std::size_t size = 0;
      for (const Property& property : element.properties) {
        size += body.minimumSize(property.list ? property.countType : property.type);
      }
      return size;
    }

    Status readElement(
        BodyReader& body, const Element& element, bool vertices, bool faces, PlyBuild& build, const MeshLimits& limits
    ) {
      if (element.properties.empty()) {
        return {};
      }
      const std::size_t instanceSize = minimumInstanceSize(body, element);
      const std::optional<std::size_t> needed = mesh_io::checkedMul(element.count, instanceSize);
      if (!needed || *needed > body.remaining()) {
        return std::unexpected("a PLY element count is larger than the file");
      }
      if (vertices) {
        return readVertices(body, element, build);
      }
      if (faces) {
        return readFaces(body, element, build, limits);
      }
      const bool fixedSize = std::ranges::none_of(element.properties, &Property::list);
      if (!body.ascii() && fixedSize) {
        return body.skip(*needed) ? Status{} : Status{std::unexpected(kEndsEarly)};
      }
      std::vector<double> scalars(element.properties.size());
      std::vector<double> items;
      // Bounded by the count, which was checked against the bytes left.
      for (std::size_t k = 0; k < element.count; ++k) {
        const auto read = readInstance(body, element, scalars, kNone, items, 0);
        if (!read) {
          return std::unexpected(read.error());
        }
      }
      return {};
    }
  } // namespace

  Mesh readPly(std::string_view bytes, const MeshLimits& limits) {
    const auto header = parseHeader(bytes);
    if (!header) {
      return meshError(std::string(header.error()));
    }
    const auto isVertex = [](const Element& element) { return element.name == "vertex"; };
    const auto isFace = [](const Element& element) { return element.name == "face"; };
    const auto vertexElement = std::ranges::find_if(header->elements, isVertex);
    const auto faceElement = std::ranges::find_if(header->elements, isFace);
    PlyBuild build;
    build.vertexTotal = vertexElement == header->elements.end() ? 0 : vertexElement->count;
    build.vertexStored = std::min({build.vertexTotal, limits.maxVertices, std::size_t{0xFFFFFFFEU}});
    build.mesh.truncated = build.vertexStored < build.vertexTotal;
    BodyReader body(bytes, header->bodyOffset, header->format);
    for (auto element = header->elements.begin(); element != header->elements.end(); ++element) {
      const Status status =
          readElement(body, *element, element == vertexElement, element == faceElement, build, limits);
      if (!status) {
        return meshError(std::string(status.error()));
      }
    }
    finishMesh(build.mesh);
    return build.mesh;
  }
} // namespace luciole::formats
