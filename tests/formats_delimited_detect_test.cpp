#include "formats/delimited_detect.h"
#include "formats_delimited_fixture.h"
#include "test_check.h"

#include <print>
#include <string>

namespace {
  using namespace luciole::formats;
  using namespace luciole::test::delimited;

  void testDetectDelimiter() {
    TEST_CHECK(detectDialect("a,b\n1,2\n").delimiter == Delimiter::Comma);
    TEST_CHECK(detectDialect("name\tsize\nx\t1\n").delimiter == Delimiter::Tab);
    TEST_CHECK(detectDialect("a|b|c\n1|2|3\n").delimiter == Delimiter::Pipe);
    TEST_CHECK(detectDialect("price;qty\n1,5;2\n3,25;4\n").delimiter == Delimiter::Semicolon);
    TEST_CHECK(detectDialect("a, b, c\n1, 2, 3\n").delimiter == Delimiter::Comma);
    // Commas and semicolons inside prose cells do not win over the real delimiter.
    TEST_CHECK(detectDialect("Hello, world\tFoo, bar\nOne, two\tThree, four\n").delimiter == Delimiter::Tab);
    TEST_CHECK(detectDialect("a,b; c\nd,e; f\n").delimiter == Delimiter::Comma);
    // Delimiters inside quoted cells do not count.
    TEST_CHECK(detectDialect("\"a;b\",\"c;d\"\n\"e;f\",g\n").delimiter == Delimiter::Comma);
    // A single column falls back to a comma, and a hint wins.
    TEST_CHECK(detectDialect("alpha\nbeta\n").delimiter == Delimiter::Comma);
    TEST_CHECK(detectDialect("a,b\n1,2\n", DialectHint{.delimiter = Delimiter::Tab}).delimiter == Delimiter::Tab);
    // A row cut by the end of the sample is ignored: counted, its thousands of commas would win.
    std::string longFile = "1,5;2,5;3,5\n";
    while (longFile.size() <= kDialectSampleBytes) {
      longFile += "x,";
    }
    longFile += "x\n";
    TEST_CHECK(detectDialect(longFile).delimiter == Delimiter::Semicolon);
  }

  void testDetectQuoteAndEscape() {
    const Dialect single = detectDialect("'a,b',c\n'd',e\n");
    TEST_CHECK(single.quote == Quote::Single);
    TEST_CHECK(single.delimiter == Delimiter::Comma);
    TEST_CHECK(detectDialect("it's,ok\nwe're,fine\n").quote == Quote::Double);
    TEST_CHECK(detectDialect("\"a\",'b'\n").quote == Quote::Double);

    const Dialect backslash = detectDialect("\"a\\\"b\",c\n\"d\",e\n");
    TEST_CHECK(backslash.escape == QuoteEscape::Backslash);
    TEST_CHECK(detectDialect("\"a\"\"b\",c\n").escape == QuoteEscape::Doubled);
    // A path ending in a backslash is not an escape, and doubled quotes win over backslashes.
    TEST_CHECK(detectDialect("\"C:\\\",x\n").escape == QuoteEscape::Doubled);
    TEST_CHECK(detectDialect("\"a\\\"b\",\"c\"\"d\"\n").escape == QuoteEscape::Doubled);
    TEST_CHECK(detectDialect("\"\",\"\"\n").escape == QuoteEscape::Doubled);
  }

  void testDetectLineEndingAndMarks() {
    const Dialect lf = detectDialect("a,b\n1,2\n");
    TEST_CHECK(lf.lineEnding == RowEnding::Lf);
    TEST_CHECK(lf.finalNewline);
    TEST_CHECK(!lf.byteOrderMark);
    TEST_CHECK(detectDialect("a,b\r\nc,d\r\n").lineEnding == RowEnding::CrLf);
    TEST_CHECK(detectDialect("a,b\rc,d\r").lineEnding == RowEnding::Cr);
    TEST_CHECK(detectDialect("a\r\nb\r\nc\n").lineEnding == RowEnding::CrLf);
    TEST_CHECK(detectDialect("a\nb\r\n").lineEnding == RowEnding::Lf);
    // A line ending inside quotes is cell text.
    TEST_CHECK(detectDialect("\"a\r\nb\r\nc\",d\ne,f\n").lineEnding == RowEnding::Lf);
    TEST_CHECK(detectDialect("a,b").lineEnding == RowEnding::Lf);

    TEST_CHECK(!detectDialect("a,b\n1,2").finalNewline);
    TEST_CHECK(detectDialect("a\r").finalNewline);

    const Dialect marked = detectDialect(std::string(kBom) + "x;y\r\n1;2\r\n");
    TEST_CHECK(marked.byteOrderMark);
    TEST_CHECK(marked.delimiter == Delimiter::Semicolon);
    TEST_CHECK(marked.lineEnding == RowEnding::CrLf);
    TEST_CHECK(marked.header);
  }

  void testDetectHeader() {
    TEST_CHECK(detectDialect("name,size\nalpha,10\nbeta,20\n").header);
    TEST_CHECK(detectDialect("when,what\n2024-01-02,a\n2024-02-03,b\n").header);
    TEST_CHECK(!detectDialect("1,2\n3,4\n").header);
    TEST_CHECK(!detectDialect("2024-01-02,a\n2024-02-03,b\n").header);
    // Same-length codes under a header of another length, and the opposite.
    TEST_CHECK(detectDialect("code\nAB1\nCD2\n").header);
    TEST_CHECK(!detectDialect("AB0\nAB1\nCD2\n").header);
    // Without typed or same-length columns: distinct, non-empty text in the first row.
    TEST_CHECK(detectDialect("alpha,beta\ngamma,delta\n").header);
    TEST_CHECK(detectDialect("name,size\n").header);
    TEST_CHECK(!detectDialect("x,\ny,zz\n").header);
    TEST_CHECK(!detectDialect("a,a\nb,cc\n").header);
    TEST_CHECK(!detectDialect("\n\n").header);
    // The caller's choice wins.
    TEST_CHECK(!detectDialect("name,size\nalpha,10\n", DialectHint{.header = false}).header);
    TEST_CHECK(detectDialect("1,2\n3,4\n", DialectHint{.header = true}).header);
  }

  void testDetectEmpty() {
    TEST_CHECK(detectDialect("") == Dialect{});
    const Dialect onlyMark = detectDialect(kBom);
    TEST_CHECK(onlyMark.byteOrderMark);
    TEST_CHECK(!onlyMark.finalNewline);
    TEST_CHECK(!onlyMark.header);
  }

} // namespace

int main() {
  testDetectDelimiter();
  testDetectQuoteAndEscape();
  testDetectLineEndingAndMarks();
  testDetectHeader();
  testDetectEmpty();
  std::println("formats delimited detect checks passed");
  return 0;
}
