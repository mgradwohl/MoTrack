# OSP XMLControl library (`motrack_osp`)

`motrack_osp` reads and writes Open Source Physics XMLControl documents, the format of every Tracker `.trk`
and of OSP preference, Data Tool and library files. It is wave 0 slice A1 of the
[migration plan](migration-plan.md#implementation-order-and-exit-gates): a lossless foundation for the
typed Tracker model. It does not yet replace the QDom readers and writers in `motrack_project`; that is
slice B1. The library has no Qt, media or operating-system dependencies (`src/osp/`, namespace
`motrack::osp`).

The reference is OSP [`d56207b`](https://github.com/OpenSourcePhysics/osp/tree/d56207b0ae737458a31365d368f1c28e4bd3202a)
(Tracker 6.3.5). The `swingJS` branch changes nothing that affects reading or writing: it adds
`writeNoSearch` and removes a debug `println`. Line numbers below refer to `XMLControlElement.java` unless
another file is named.

## OSP's reader is line-oriented

OSP does not use an XML parser. `readInput`, `readObject` and `readProperty` call
`BufferedReader.readLine()` and test each line with `String.indexOf`. MoTrack ports that reader rather than
approximating it with an XML DOM, so the tree it builds is the tree Tracker builds:

| Behavior | OSP source |
| --- | --- |
| `\n`, `\r\n` and a lone `\r` end lines; U+2028, U+2029 and U+0085 do not. | `BufferedReader.readLine` |
| The root is the first line containing `<object class=` among the first 10 lines; otherwise the read fails. | 1581–1599 |
| The root's `version="..."` is taken from that line, even from an XML declaration on the same line. | 1595 |
| Any `/>` on an object's opening line makes it self-closing, and the next line is discarded. | 1689–1691 |
| A line containing `</object>` closes the object and the next line is discarded (normally `</property>`). | 1698–1700 |
| Inside an object, only lines containing `<property` matter; blank lines, comments and stray text are ignored. | 1703–1706 |
| An array or collection reads consecutive `<property` lines and consumes the first other line, which may be a blank line inside the list; later elements then become properties of the enclosing object. Without `/>` on its opening line, `...></property>` keeps consuming the following property lines. | 1743–1754 |
| An object property whose line contains `>null</property` is null; otherwise the **next line** is read as its object's opening tag, whatever it contains. | 1757–1761 |
| A null object property directly in an object makes the whole read fail (`setProperty` calls `content.get(0)`). Null objects are only valid as array or collection elements. | 1541 |
| A string is read through CDATA when its line contains `<![CDATA[` anywhere, and ends only at `]]></property>`, so `]]>` and `</property>` can occur inside it. | 1763–1772 |
| Other text starts after the line's first `>` (or at the line start) and ends at the first `</property>`, so a self-closing string swallows lines up to the next `</property>`. Text after the end tag, including a second property on the same line, is ignored. | 1780–1784 |
| Multi-line text is joined with the platform line separator. MoTrack joins with `\n`. | 1768, 1782 |
| No entity decoding: `&amp;` stays `&amp;`. | — |
| Attributes are found with `indexOf("name=\"")`, so order does not matter and a name such as `atype=` can supply the `type` attribute. A missing name is allowed; a missing type or an unterminated name/type/version quote fails the read; an unterminated class gives class `""`. | `XML.getAttr`, 1738–1740, 2143 |
| Classes in a package ending with `org.opensourcephysics.media` move to its `.core` subpackage. | 2143–2157 |
| Every duplicate property is kept in order; name lookup returns the last; `getChildControl` returns the last *object* property of that name. | 1711–1715, 1534–1545 |
| A `Cryptic` root is decrypted by OSP. MoTrack reports it as unsupported. | 1607–1639 |

Typed values are decoded lazily with Java's own conversions: `Integer.parseInt`, `Double.parseDouble`
(trimmed, with `NaN`, `Infinity`, hexadecimal significands and `f`/`d` suffixes, correctly rounded),
`"true".equals` for booleans and `XML.removeCDATA` for strings. Compact primitive arrays follow
`arrayValue(String, Class)` (line 2002): Java's `String.split(",")` drops trailing empty fields, so
`{1,2,,}` is two ints and `{,}` is empty, while `{}` and `{1,,2}` fail for numbers and `{}` is `[false]` for
booleans; nested rows separated by a bare comma are null and a trailing comma adds none.

## Data model

`parse(bytes)` returns an immutable `Source`: the original bytes, the line table, and a node table of
objects and properties. Each node records the raw attribute text, the effective class, the logical text
content, CDATA/null/self-closing/closed flags, ordered children, and the lines it consumed (opening line,
closing line and last line, including a discarded line). `ObjectView` and `PropertyView` provide
`XMLControlElement`-style lookups (`property`, `child`, `getInt`, `getDouble`, `getBoolean`, `getString`) and
exact decoders (`intValue`, `doubleValue`, `booleanValue`, `stringValue`, `compactArray`,
`indexedElements`). Decoders return `std::expected` with a `DecodeError` instead of Java's
`Integer.MIN_VALUE`, `NaN`, `false` or `null`, so callers can tell a missing value from a malformed one.

Inputs larger than 32 MiB, deeper than 256 levels or with more than two million nodes are refused. Input
must be valid UTF-8; a byte-order mark is kept as text. OSP ignores the XML declaration and decodes UTF-8; MoTrack
reads the `encoding` attribute of the declaration only (up to `?>` or the first `<`, `>` or line break) and
refuses a document only when it declares another encoding **and** contains non-ASCII bytes, which that
encoding could mean differently. A document whose objects reach end of file before `</object>` is refused
with `Truncated` (#243); `ParseLimits::acceptTruncated` reads it as OSP does, for diagnostics and the
oracle only. Compact arrays are limited to the JVM's 255 dimensions and their braces are matched in one
linear pass. `indexedElements()` stores only the elements present, so a huge `[n]` index allocates nothing.
A `Patch` is constructed for one `Source` and `render` refuses it for any other.

## Editing and writing

A `Patch` holds edits against node ids: set a text value (`setInt`, `setDouble`, `setBoolean`, `setString`,
`setContent`), insert into an object/array/collection, remove a property and replace a property subtree.
`ObjectValue`/`PropertyValue` describe new content; builders follow `XMLPropertyElement`'s writer, including
`[j]` element names with null entries skipped except the last, `item` collection elements, and compact
`{a,b}` primitive arrays when Java's element count is positive (`compactArraySize = 0`).

`render(source, patch)` splices the original bytes: untouched bytes are copied verbatim, a changed value
replaces only the text between the opening line's first `>` and its `</property>`, and new or replaced
subtrees are written in OSP's layout (an object shares its property's indentation, nested properties add
four spaces) below the indentation of the replaced line or of a sibling, with the file's first line
ending. Strings get CDATA when they contain `" < > & '` (`XML.requiresCDATA`), decided from the logical value
on every write, so a loaded CDATA string is never written bare as `toXML()` of a loaded control does
(osp#21). Attribute values may not contain quotes, angle brackets or line breaks, or end in `=` (which would
form `type="` or `class="` with the closing quote). A self-closing object or array
that receives children is reopened by replacing its first `/>`. Values OSP could not read back are refused,
for example names with quotes or angle brackets, `]]></property>` in a string, or a null object directly in
an object. Every render is re-parsed with the same reader and must produce exactly the intended logical
tree; otherwise it returns `VerificationFailed`. `write(root)` produces a new document as
`XMLControlElement.toString` does, with an XML declaration and no final line break, and is verified the same
way.

Doubles are formatted as JDK 19+ `Double.toString` does: the shortest decimal that rounds back (two digits
when one would suffice, as in `4.9E-324`), plain notation for 1e-3 ≤ |x| < 1e7 and `1.0E7` otherwise. The
conversion uses exact big-integer arithmetic and no locale.

Results use `motrack::osp::Expected`, which is `std::expected` where the standard library provides it.
libstdc++ hides `<expected>` from Clang 18 (it requires `__cpp_concepts >= 202002L`), so
[`expected.hpp`](../src/osp/expected.hpp) supplies the small subset the library uses on that toolchain; the
unit tests pass with both forms.

## Oracles

**O2** — rendering an unedited parse reproduces the input byte for byte. `motrack_tests` checks it for every
`.trk`/`.xml` under `tests/fixtures` and `tests/corpus/fixtures`, including members of `.trz` archives, in
their own, LF, CRLF and CR line endings. The probe checks it for every document of the O1 run.

**O1** — MoTrack's tree equals the tree of the pinned Java `XMLControlElement`.
[`XmlControlDump.java`](../tools/reference/XmlControlDump.java) reads each document with Tracker's resource
reader (UTF-8) and `motrack_corpus_probe --osp-dump in.xml out.json` dumps MoTrack's parse in the same
schema. `python tools/corpus.py xml-oracle --java JAVA --probe PROBE [--cache DIR] [--reports DIR]` compares
the two for every manifest case file and XML archive member (including fetched real experiments), the
committed fixtures, and the synthetic reader fixtures in [`tests/fixtures/osp`](../tests/fixtures/osp) in LF,
CRLF, CR and BOM+CRLF variants. It runs on Linux CI after the build. Mismatching dumps and a `summary.json`
are written to the report directory.

`XmlControlDump.java --generate DIR` writes documents with OSP's own writer: the document built by OSP's
`src/test/SetXMLControlTest.java` demo (without its dialog), one covering the remaining writer branches
(CDATA, empty and multi-line strings, empty and null-row primitive arrays, `[j]` arrays with skipped nulls,
collections and a nested `java.awt.Point`), and the jagged arrays OSP cannot read back (#193). Their committed
copies in [`tests/fixtures/osp/generated`](../tests/fixtures/osp/generated) must equal a fresh run and they are
O1/O2 inputs. `motrack_tests` requires MoTrack's `write()` to reproduce the first two byte for byte. OSP's other demo,
`src/test/LoaderTest.java`, opens ComPADRE library resources in a text editor and builds no XMLControl
document of its own.

Dump schema `motrack.osp-xmlcontrol-dump/1`:

```json
{"schema": "motrack.osp-xmlcontrol-dump/1", "read": true, "root": {
  "class": "effective class", "version": "root only, or null",
  "properties": [
    {"name": "or null", "type": "int|double|boolean|string|unknown", "content": "logical text",
     "int": 5, "double": "16 hex digits of doubleToLongBits, or null", "boolean": false, "string": "removeCDATA, or null"},
    {"name": "...", "type": "array|collection", "class": "effective class", "elements": [],
     "compact": "decoded compact array when the class is [I/[D/[Z (any depth) and the first element is a text property named array"},
    {"name": "...", "type": "object", "object": {"class": "...", "properties": []}}]}}
```

Only the typed key matching `type` is present; `null` means Java's conversion throws. A document either
reader rejects is `{"schema": ..., "read": false}`. MoTrack's dump may add `"motrack_rejects": "<reason>"` when
it deliberately refuses a document that OSP reads; it then dumps the tree it would read with the refusal
lifted. The runner removes the key, still requires the tree to equal Java's, and accepts only reasons in its
registry (`truncated`, #243). `xml-oracle --o2-only` runs the probe over the same documents without Java and
runs in CI on Windows and macOS; O1 needs the Java reference and runs on Linux.

### Intentional differences

The dump compares only what Java keeps. MoTrack also retains, and does not dump: raw attribute text
(including an unknown type's attribute and the class before the media package move), the CDATA flag, line
spans, ignored and discarded lines, the prolog and epilog, and whether an object reached end of file
unclosed. Where Java's behavior is unusable, MoTrack reports an error instead and the oracle fixtures avoid
the case:

- End of file inside a text value: Java appends `"null"` forever until it runs out of memory; MoTrack returns
  `UnterminatedValue` ([#195](https://github.com/mgradwohl/MoTrack/issues/195), [osp#22](https://github.com/OpenSourcePhysics/osp/issues/22)).
- OSP writes compact arrays its own reader decodes differently: an empty later row (`{{1,2},{}}` throws, an
  empty boolean row reads as `[false]`) and a trailing null row (`{{1,2},null}` reads as `[[1,2]]`). MoTrack's
  reader mirrors that; its writer uses compact text only when that decoder reproduces the values and the
  `[j]` element form otherwise ([#193](https://github.com/mgradwohl/MoTrack/issues/193),
  [osp#20](https://github.com/OpenSourcePhysics/osp/issues/20); evidence in `generated/osp-jagged-arrays.xml`).
- Strings containing `]]></property>` or `<![CDATA[` cannot be represented in OSP's format: OSP writes them
  and then truncates them or fails in `XML.removeCDATA` on reload. MoTrack refuses to write them
  (`InvalidValue`), including multi-line values that would inject a property
  ([#194](https://github.com/mgradwohl/MoTrack/issues/194), [osp#21](https://github.com/OpenSourcePhysics/osp/issues/21)).
- Invalid UTF-8: Java substitutes U+FFFD; MoTrack refuses the file. A declared encoding other than UTF-8 is
  ignored by OSP but refused by MoTrack.
- Encrypted (`Cryptic`) roots are refused instead of decrypted.
- `Integer.parseInt` accepts non-ASCII Unicode decimal digits; MoTrack reports them as `Unsupported`.
- Nesting beyond 256 levels is refused; Java fails later with a stack overflow.
- Tracker 6.3.5 runs on JDK 17, whose `Double.toString` can print longer, non-shortest digit strings for some
  values. MoTrack writes the JDK 19+ form; both parse to the same double.
- MoTrack only writes UTF-8 with the chosen line ending and refuses content OSP would misread, where OSP
  would write a file that it cannot read back.
- A document whose objects reach end of file loads partially in OSP; MoTrack refuses it (#243).
- For future O7 checks of MoTrack's own writes against Tracker resaves, doubles must be compared by value (or a
  JDK 17 formatting mode added): MoTrack writes JDK 19+ shortest digits, while Tracker's JDK 17 prints, for
  example, `2e23` as `1.9999999999999998E23`.
