#include "formats/xml_reader.h"
#include "test_check.h"

#include <cstddef>
#include <ctime>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace {
  using namespace luciole::formats;

  // Upper bound on events read from one test document.
  constexpr std::size_t kMaxTestEvents = 10'000;

  // One event as compact text: "<name a=v>", "</name>", "t(text)", "c(text)", "end", or "!" plus the error.
  std::string show(const std::expected<XmlEvent, XmlError>& event) {
    if (!event) {
      return "!" + std::to_string(static_cast<int>(event.error()));
    }
    switch (event->kind) {
    case XmlEventKind::StartElement: {
      std::string out = "<" + std::string(event->name);
      for (const XmlAttribute& attribute : event->attributes) {
        out += " " + std::string(attribute.name) + "=" + std::string(attribute.value);
      }
      return out + ">";
    }
    case XmlEventKind::EndElement:
      return "</" + std::string(event->name) + ">";
    case XmlEventKind::Text:
      return "t(" + std::string(event->text) + ")";
    case XmlEventKind::CData:
      return "c(" + std::string(event->text) + ")";
    case XmlEventKind::End:
      return "end";
    }
    return "?";
  }

  std::string errorText(XmlError error) { return "!" + std::to_string(static_cast<int>(error)); }

  // Every event up to and including the first End or error.
  std::vector<std::string> events(std::string_view bytes, const XmlLimits& limits = {}) {
    XmlReader reader(bytes, limits);
    std::vector<std::string> out;
    for (std::size_t k = 0; k < kMaxTestEvents; ++k) {
      const auto event = reader.next();
      out.push_back(show(event));
      if (!event || event->kind == XmlEventKind::End) {
        break;
      }
    }
    return out;
  }

  // The error that ends the document, or nothing when it reaches End.
  std::expected<void, XmlError> finalResult(std::string_view bytes, const XmlLimits& limits = {}) {
    XmlReader reader(bytes, limits);
    for (std::size_t k = 0; k < kMaxTestEvents; ++k) {
      const auto event = reader.next();
      if (!event) {
        return std::unexpected(event.error());
      }
      if (event->kind == XmlEventKind::End) {
        return {};
      }
    }
    return std::unexpected(XmlError::TooManyEvents);
  }

  using Events = std::vector<std::string>;

  void testEveryEventKind() {
    const std::string bytes = "<?xml version=\"1.0\"?>\n<!-- note -->\n<root a=\"1\" p:b='two'><p:child/>text"
                              "<![CDATA[<raw>&amp;]]><e></e></root>\n";
    TEST_CHECK((
        events(bytes)
        == Events{
            "<root a=1 p:b=two>", "<p:child>", "</p:child>", "t(text)", "c(<raw>&amp;)", "<e>", "</e>", "</root>", "end"
        }
    ));
    XmlReader reader(bytes);
    for (std::size_t k = 0; k < 9; ++k) {
      TEST_CHECK(reader.next().has_value());
    }
    // End repeats once reached.
    const auto again = reader.next();
    TEST_CHECK(again.has_value() && again->kind == XmlEventKind::End);
    TEST_CHECK(reader.depth() == 0);
  }

  void testDepthAndAttributesOfStartEvent() {
    XmlReader reader(R"(<a><b x="1" y="2"/></a>)");
    TEST_CHECK(reader.next().has_value());
    TEST_CHECK(reader.depth() == 1);
    const auto b = reader.next();
    TEST_CHECK(b.has_value() && b->kind == XmlEventKind::StartElement && b->name == "b");
    TEST_CHECK(b->attributes.size() == 2);
    TEST_CHECK(reader.depth() == 2);
    const auto endB = reader.next();
    TEST_CHECK(endB.has_value() && endB->kind == XmlEventKind::EndElement && endB->name == "b");
    TEST_CHECK(reader.depth() == 1);
  }

  void testWhitespaceTextInsideElementsIsReported() {
    TEST_CHECK((events("<a> <b/>\n</a>") == Events{"<a>", "t( )", "<b>", "</b>", "t(\n)", "</a>", "end"}));
  }

  void testPredefinedEntitiesAndCharacterReferences() {
    TEST_CHECK(
        (events("<a>&lt;&gt;&amp;&apos;&quot;&#65;&#x42;&#xE9;&#x1F600;</a>")
         == Events{"<a>", "t(<>&'\"AB\xC3\xA9\xF0\x9F\x98\x80)", "</a>", "end"})
    );
    TEST_CHECK((events("<a v=\"&lt;&amp;&#x20AC;\"/>") == Events{"<a v=<&\xE2\x82\xAC>", "</a>", "end"}));
  }

  void testLineEndingsAndAttributeWhitespace() {
    TEST_CHECK((events("<a>x\r\ny\rz</a>") == Events{"<a>", "t(x\ny\nz)", "</a>", "end"}));
    TEST_CHECK((events("<a v=\"1\t2\n3\r\n4\"/>") == Events{"<a v=1 2 3 4>", "</a>", "end"}));
    // A character reference keeps the character it names.
    TEST_CHECK((events("<a v=\"1&#10;2\"/>") == Events{"<a v=1\n2>", "</a>", "end"}));
  }

  void testBadReferences() {
    TEST_CHECK(finalResult("<a>&foo;</a>") == std::unexpected(XmlError::UnknownEntity));
    TEST_CHECK(finalResult("<a>&;</a>") == std::unexpected(XmlError::UnknownEntity));
    TEST_CHECK(finalResult("<a v=\"&nbsp;\"/>") == std::unexpected(XmlError::UnknownEntity));
    TEST_CHECK(finalResult("<a>&#0;</a>") == std::unexpected(XmlError::BadCharacterReference));
    TEST_CHECK(finalResult("<a>&#xD800;</a>") == std::unexpected(XmlError::BadCharacterReference));
    TEST_CHECK(finalResult("<a>&#x110000;</a>") == std::unexpected(XmlError::BadCharacterReference));
    TEST_CHECK(finalResult("<a>&#99999999999999999999;</a>") == std::unexpected(XmlError::BadCharacterReference));
    TEST_CHECK(finalResult("<a>&#x;</a>") == std::unexpected(XmlError::BadCharacterReference));
    TEST_CHECK(finalResult("<a>&#12a;</a>") == std::unexpected(XmlError::BadCharacterReference));
    TEST_CHECK(finalResult("<a>&amp</a>") == std::unexpected(XmlError::Malformed));
  }

  void testCDataKeepsMarkupAndReferences() {
    TEST_CHECK((events("<a><![CDATA[]]></a>") == Events{"<a>", "c()", "</a>", "end"}));
    TEST_CHECK((events("<a><![CDATA[x]y]]z]]></a>") == Events{"<a>", "c(x]y]]z)", "</a>", "end"}));
    TEST_CHECK(finalResult("<![CDATA[x]]><a/>") == std::unexpected(XmlError::Malformed));
  }

  void testCommentsAndProcessingInstructionsAreSkipped() {
    TEST_CHECK((events("<a><!-- <b> --><?pi <c>?>x</a><!-- after -->") == Events{"<a>", "t(x)", "</a>", "end"}));
  }

  void testDoctypeIsSkipped() {
    const std::string bytes = "<!DOCTYPE r SYSTEM \"x>y.dtd\" [\n"
                              "  <!ENTITY e \"]>\">\n"
                              "  <!-- ]> -->\n"
                              "  <?pi ]> ?>\n"
                              "  <!ATTLIST r a CDATA '>'>\n"
                              "]><r>ok</r>";
    TEST_CHECK((events(bytes) == Events{"<r>", "t(ok)", "</r>", "end"}));
    // A declared entity is never expanded.
    const std::string_view root = "<r>ok</r>";
    TEST_CHECK(
        finalResult(bytes.substr(0, bytes.size() - root.size()) + "<r>&e;</r>")
        == std::unexpected(XmlError::UnknownEntity)
    );
    TEST_CHECK(finalResult("<r/><!DOCTYPE r>") == std::unexpected(XmlError::Malformed));
    TEST_CHECK(finalResult("<!DOCTYPE r><!DOCTYPE r><r/>") == std::unexpected(XmlError::Malformed));
  }

  void testEntityBombIsNeverExpanded() {
    std::string bytes = "<?xml version=\"1.0\"?>\n<!DOCTYPE lolz [\n<!ENTITY lol \"lol\">\n";
    for (int level = 1; level <= 9; ++level) {
      bytes += "<!ENTITY lol" + std::to_string(level) + " \"";
      for (int k = 0; k < 10; ++k) {
        bytes += level == 1 ? "&lol;" : "&lol" + std::to_string(level - 1) + ";";
      }
      bytes += "\">\n";
    }
    bytes += "]>\n<lolz>";
    const std::clock_t start = std::clock();
    TEST_CHECK((events(bytes + "</lolz>") == Events{"<lolz>", "</lolz>", "end"}));
    TEST_CHECK((events(bytes + "&lol9;</lolz>") == Events{"<lolz>", errorText(XmlError::UnknownEntity)}));
    TEST_CHECK(static_cast<double>(std::clock() - start) / CLOCKS_PER_SEC < 1.0);
  }

  void testUnterminatedAtEveryPosition() {
    const std::string bytes = "<?xml version=\"1.0\"?><!DOCTYPE r [<!ENTITY e \"x\">]><r a=\"1&amp;2\" b='&#x41;'>"
                              "<!-- c --><x:s/>t&lt;<![CDATA[d]]><?pi x?>&#65;</r>";
    TEST_CHECK(finalResult(bytes).has_value());
    for (std::size_t size = 0; size < bytes.size(); ++size) {
      const auto result = finalResult(std::string_view(bytes).substr(0, size));
      TEST_CHECK(result == std::unexpected(XmlError::Unterminated));
    }
    // Ends after an end tag's name, after an attribute's name or '=', and inside a comment in a DOCTYPE.
    for (const std::string_view cut : {"<a></a ", "<a b ", "<a b= ", "<!DOCTYPE a [<!-- ]>"}) {
      TEST_CHECK(finalResult(cut) == std::unexpected(XmlError::Unterminated));
    }
  }

  void testMalformedMarkup() {
    TEST_CHECK(finalResult("<a></b>") == std::unexpected(XmlError::MismatchedTag));
    TEST_CHECK(finalResult("</a>") == std::unexpected(XmlError::MismatchedTag));
    TEST_CHECK(finalResult("<a/><b/>") == std::unexpected(XmlError::Malformed));
    TEST_CHECK(finalResult("x<a/>") == std::unexpected(XmlError::Malformed));
    TEST_CHECK(finalResult("<a/>x") == std::unexpected(XmlError::Malformed));
    TEST_CHECK(finalResult("<a b=1/>") == std::unexpected(XmlError::Malformed));
    TEST_CHECK(finalResult("<a b=\"<\"/>") == std::unexpected(XmlError::Malformed));
    TEST_CHECK(finalResult("<a b=\"1\"c=\"2\"/>") == std::unexpected(XmlError::Malformed));
    TEST_CHECK(finalResult("<a b/>") == std::unexpected(XmlError::Malformed));
    TEST_CHECK(finalResult("< a/>") == std::unexpected(XmlError::Malformed));
    TEST_CHECK(finalResult("<a/ >") == std::unexpected(XmlError::Malformed));
    TEST_CHECK(finalResult("<a></a b>") == std::unexpected(XmlError::Malformed));
    TEST_CHECK(finalResult("<!ELEMENT a><a/>") == std::unexpected(XmlError::Malformed));
    // Whitespace around the root and inside an end tag is allowed, and a leading byte order mark is skipped.
    TEST_CHECK(finalResult(" \n<a></a >\n").has_value());
    TEST_CHECK((events("\xEF\xBB\xBF<a/>") == Events{"<a>", "</a>", "end"}));
    TEST_CHECK(finalResult("<a>\xEF\xBB\xBF</a>").has_value());
  }

  void testErrorsAreSticky() {
    XmlReader reader("<a></b><c/>");
    TEST_CHECK(reader.next().has_value());
    TEST_CHECK(reader.next() == std::unexpected(XmlError::MismatchedTag));
    TEST_CHECK(reader.next() == std::unexpected(XmlError::MismatchedTag));
  }

  void testDepthLimit() {
    const XmlLimits limits{.maxDepth = 2};
    TEST_CHECK(finalResult("<a><b/></a>", limits).has_value());
    TEST_CHECK((events("<a><b><c/></b></a>", limits) == Events{"<a>", "<b>", errorText(XmlError::TooDeep)}));
  }

  void testAttributeLimit() {
    const XmlLimits limits{.maxAttributes = 2};
    TEST_CHECK(finalResult("<a x=\"1\" y=\"2\"/>", limits).has_value());
    TEST_CHECK(finalResult("<a x=\"1\" y=\"2\" z=\"3\"/>", limits) == std::unexpected(XmlError::TooManyAttributes));
  }

  void testNameLimit() {
    const XmlLimits limits{.maxNameBytes = 3};
    TEST_CHECK(finalResult("<abc xyz=\"1\"></abc>", limits).has_value());
    TEST_CHECK(finalResult("<abcd/>", limits) == std::unexpected(XmlError::NameTooLong));
    TEST_CHECK(finalResult("<a wxyz=\"1\"/>", limits) == std::unexpected(XmlError::NameTooLong));
    TEST_CHECK(finalResult("<a></abcd>", limits) == std::unexpected(XmlError::NameTooLong));
  }

  void testValueLimit() {
    const XmlLimits limits{.maxValueBytes = 3};
    TEST_CHECK(finalResult("<a v=\"abc\">abc<![CDATA[abc]]>&amp;&amp;&amp;</a>", limits).has_value());
    TEST_CHECK(finalResult("<a>abcd</a>", limits) == std::unexpected(XmlError::ValueTooLong));
    TEST_CHECK(finalResult("<a v=\"abcd\"/>", limits) == std::unexpected(XmlError::ValueTooLong));
    TEST_CHECK(finalResult("<a><![CDATA[abcd]]></a>", limits) == std::unexpected(XmlError::ValueTooLong));
    TEST_CHECK(finalResult("<a>&#x20AC;&#x20AC;</a>", limits) == std::unexpected(XmlError::ValueTooLong));
  }

  void testEventLimit() {
    const XmlLimits limits{.maxEvents = 4};
    TEST_CHECK(finalResult("<a><b/></a>", limits).has_value());
    TEST_CHECK(
        (events("<a><b/>x</a>", limits) == Events{"<a>", "<b>", "</b>", "t(x)", errorText(XmlError::TooManyEvents)})
    );
  }

  void testNameHelpers() {
    TEST_CHECK(localName("x:sheet") == "sheet");
    TEST_CHECK(localName("sheet") == "sheet");
    TEST_CHECK(localName("a:b:c") == "b:c");
    TEST_CHECK(prefix("x:sheet") == "x");
    TEST_CHECK(prefix("sheet").empty());
    TEST_CHECK(hasLocalName("x:sheet", "sheet"));
    TEST_CHECK(hasLocalName("sheet", "sheet"));
    TEST_CHECK(!hasLocalName("x:sheets", "sheet"));

    const std::vector<XmlAttribute> attributes{
        {.name = "sheetId", .value = "1"},
        {.name = "r:id", .value = "rId1"},
        {.name = "office:value-type", .value = "float"},
        {.name = "calcext:value-type", .value = "error"},
    };
    TEST_CHECK(attributeValue(attributes, "r:id") == "rId1");
    TEST_CHECK(attributeValue(attributes, "calcext:value-type") == "error");
    TEST_CHECK(!attributeValue(attributes, "id").has_value());
    TEST_CHECK(attributeValueByLocalName(attributes, "id") == "rId1");
    TEST_CHECK(attributeValueByLocalName(attributes, "value-type") == "float");
    TEST_CHECK(!attributeValueByLocalName(attributes, "name").has_value());
  }
} // namespace

int main() {
  testEveryEventKind();
  testDepthAndAttributesOfStartEvent();
  testWhitespaceTextInsideElementsIsReported();
  testPredefinedEntitiesAndCharacterReferences();
  testLineEndingsAndAttributeWhitespace();
  testBadReferences();
  testCDataKeepsMarkupAndReferences();
  testCommentsAndProcessingInstructionsAreSkipped();
  testDoctypeIsSkipped();
  testEntityBombIsNeverExpanded();
  testUnterminatedAtEveryPosition();
  testMalformedMarkup();
  testErrorsAreSticky();
  testDepthLimit();
  testAttributeLimit();
  testNameLimit();
  testValueLimit();
  testEventLimit();
  testNameHelpers();
  return 0;
}
