# Digital Library collections (`motrack_library`)

`motrack_library` models the Open Source Physics Digital Library: collection files (`LibraryResource` and
`LibraryCollection` XMLControl documents), "my library" files (`Library`) and ComPADRE REST responses. It is
wave 0 slice L1 of the [migration plan](migration-plan.md#implementation-order-and-exit-gates) and the data
layer of the Library Browser (slice I6, requirement R24), which adds network access, search, the cache and the
UI. The library has no Qt, network or file-system dependency (`src/library/`, namespace `motrack::library`) and
reads and writes OSP documents through [`motrack_osp`](osp-xmlcontrol.md).

The reference is OSP [`d56207b`](https://github.com/OpenSourcePhysics/osp/tree/d56207b0ae737458a31365d368f1c28e4bd3202a)
(Tracker 6.3.5). The `swingJS` tip (`d49c6ec`) differs only in reading a `resources` ArrayList (below) and in
browser code; the save format is unchanged ([upstream delta](https://github.com/mgradwohl/MoTrack/blob/main/docs/upstream-delta.md) U20, F7).

## Model

`Resource` holds what OSP's loaders leave in a `LibraryResource` or `LibraryCollection`; `kind` says which class.
`Library` holds a `Library` file as declared. Setters such as `setTarget` and `addMetadata` have OSP's side
effects, so the model stays in the state Java would be in.

| Property | Field | Loading (LibraryResource.Loader) | Writing (saveObject) |
| --- | --- | --- | --- |
| `name` | `name` | `getString`, trimmed; missing is "" | always |
| `description` | `description` (HTML) | trimmed | when not empty |
| `base_path` | `basePath` | trimmed | when not empty |
| `target` | `target` | **not trimmed**, no type from the extension | when not empty |
| `html_path` | `htmlPath` | trimmed; a resource without a target becomes HTML | when not empty and not inside the target archive |
| `type` | `type` | only the exact names Unknown, Collection, Tracker, EJS, Video, Image, HTML, PDF, Data, URL; collections ignore it | unless Unknown; never for a collection |
| `metadata` | `metadata` | `String[][]` rows; see below | when not empty |
| `properties` | `properties` | `String[][]` rows into a sorted map (last value wins, values may be null) | when not empty |
| `resources` | `children` | `LibraryResource[]`; null and unknown-class elements are skipped | collections with children |

`getString` returns null for a missing property or another type, and the last property of a name wins.
Metadata go through `addMetadata`: the keys generator, progid, originator, referrer, theme-color,
color-scheme, viewport, googlebot and robots are dropped (ASCII case-insensitive); exactly "author", "contact" and
"keywords" become "Author", "Contact" and "Keywords"; pairs are sorted by key, then value, in Java string order
(UTF-16 units), and equal pairs collapse. A collection without a decodable `resources` value sets its target
again, which trims it. The thumbnail is never saved or loaded; ComPADRE records set it to their thumbnail URL.
`download_filename` and `reload_url` are ComPADRE's entries in `properties`.

OSP's loader throws where Java's casts and array accesses fail. MoTrack returns `LoadError::Malformed` with the
property path, for example `root.resources[3]`: a metadata or properties row with fewer than two entries, a null
row or property name, an array of the wrong class, an array element index out of range (the last element in the
file sets the length), an element of the wrong type, or malformed CDATA. Elements of a class Java cannot load
read as null; MoTrack treats every class except the library classes and `java.*`/`javax.*` that way, keeps the
element, and reports a `Diagnostic`.

## Paths

`absoluteTarget` and `absoluteHtmlPath` resolve against the first non-empty base path from the resource up to
the root, with `XML.getResolvedPath` (`resolvedPath`):

| Relative | Base | Result |
| --- | --- | --- |
| `a.trk` | `https://host/dir/` or `https://host/dir` | `https://host/dir/a.trk` (one trailing `/` is dropped) |
| `../a.trk` | `https://host/dir/sub` | `https://host/dir/a.trk` |
| `../../a.trk` | `https://host/dir` | `https:/a.trk`: `../` cuts at the last `/`, even inside `https://` |
| `..\Expériences\chute libre é.trk` | `C:\Users\Student\Tracker Library` | `C:/Users/Student/Expériences/chute libre é.trk` |
| `D:\Data\run 1.trk`, `\\server\share\lab.trz`, `file:/x.trk` | any | absolute: a leading `/` or any `:/`, after backslashes become `/` |
| `C:x.trk` | `/base` | `/base/C:x.trk` (no `:/`, so relative) |
| `./../a.trk` | `/b/sub` | `/b/sub/../a.trk` (one leading `./` is removed after the `../` loop; nothing is normalized) |
| `&OSPSubject=223` | ComPADRE records query | concatenated without `/` |
| `pendulum.trz!/html/info.html` | `https://host/exp/` | `https://host/exp/pendulum.trz!/html/info.html` |

Spaces, `%20` and non-ASCII text are kept as written. An archive member is `archive!/member`
(`splitArchiveMember`); `isJarZipTrz` tests `.jar`, `.zip` and `.trz` case-sensitively. The Library Browser
(`browserAbsoluteTarget`, `browserHtmlPath`) falls back to the directory of the collection file or URL when no
ancestor has a base path, and appends a populated collection's target to its base path without a separator.
`uriPath` and `nonUriPath` are ResourceLoader's conversions between paths and `file:`/`%20` forms; the platform
choice (`file:/` on Windows, `file://` elsewhere) is a parameter.

`relativePath`, used when authoring, follows `XML.getPathRelativeTo` (forward slashes, at most five `../`
levels, unrooted paths unchanged) but matches the base on whole segments: OSP turns `C:/library/a.html` relative
to `C:/lib` into `rary/a.html` (#265).

## Saving

`saveCollection` renders a `Patch` against the parsed baseline, so an unedited document is reproduced byte for
byte and every property MoTrack does not model survives. A changed field edits the text of its property,
inserts the property in OSP's order, or removes it where OSP would not write it. Removing children deletes their
array elements (and shadowed duplicates of the same index); children added after the existing ones are appended
with the next indices. Reordering or inserting earlier rewrites that `resources` array from each child's original
logical content, keeping elements Java ignores. A changed class replaces the element, or rewrites the document at
the root. Resources copied from another document keep their unknown properties. Each output is reloaded and must
equal the model as OSP loads it.

Two choices keep MoTrack's saves lossless where OSP's writer is not: an HTML path inside the target archive,
which OSP drops and derives again from the archive, is kept; and a resource whose type is Unknown but which has an
HTML page and no target gets an explicit `Unknown` type, because the loader would otherwise make it HTML.
`writeCollection` is `new XMLControlElement(resource).toXML()` for new documents: exactly OSP's layout and rules.

## ComPADRE

`comPadreCollection(query, xml)` is `LibraryComPADRE.getCollection`: its name and info page follow the query, an
"About OSP and ComPADRE" page comes first, `loadComPadreTree` adds a collection for each `sub-tree` of the
`osp-subject` set (recursively through `-detail` sets) with the accumulated `&param` query as target, and the
base path is the record query (Tracker's, otherwise EJS's, plus `&OSPPrimary=Subject` when asked). Parameter
values go through `nonUriPath`, so `%20` becomes a space and `&`/`?` become `_`; an `Unclassified` sub-tree's
query becomes the parent's target. `loadComPadreRecords` maps a record response the way `loadNode`/`setRecord`
do with Tracker's desired type "Tracker": title, download URL (`&amp;` decoded once more), type Tracker,
`download_filename`, `reload_url`, thumbnail URL, Author and Keywords metadata (subjects split at ` / `,
"General" skipped) and the HTML description of `LibraryResource.getHTMLBody`, with ToolsRes labels and a
`0.0`-pattern HALF_EVEN megabyte size. The REST XML is read by a bounded DOM parser equivalent to Java's default
`DocumentBuilder` (text merged across references, CDATA and comments kept as separate nodes); document type
declarations and encodings other than UTF-8 are refused.

`ComPadreMode::MoTrack` (the default) differs deliberately: each record opens the attachment with its own
`file-identifier` before Tracker's Main/Primary, then Supplemental order, so records whose own file is a
supplemental movie or a Source Code TRZ open correctly (#213); and a record without an identifier, file size,
contributors or text no longer ends the list or loses its description (#266). `ComPadreMode::Tracker635`
reproduces Tracker for the oracle. Desktop Tracker shows a cached thumbnail; the browser maps the URL to its cache.

## Libraries

`loadLibrary` follows `Library.Loader`: collection paths keep duplicates and take their last name, a null path or
name is skipped, fewer names than paths fails (as does a names list longer than `recently_opened`), recently
opened paths go through `addRecent` (no duplicates, at most six), and `no_search_paths` is sorted. MoTrack keeps
declared sub-library and imported paths instead of opening them, and a missing name stays null instead of
"<home folder> Library". `saveLibrary` rewrites only changed lists and derives `recently_opened_names` as OSP does.

## Oracle

[`LibraryDump.java`](../tools/reference/LibraryDump.java) loads each document with the pinned OSP classes
(`XMLControlElement`, the library loaders, and `LibraryComPADRE.load`/`loadNode` offline with
`desiredOSPType = "Tracker"`) and dumps the tree, every resource's resolved paths, and the document written back
by `toXML`. `motrack_corpus_probe --library-dump KIND input output.json` writes MoTrack's dump in the same schema
(`motrack.library-dump/1`) and also requires an unedited save to be byte-identical. `python tools/corpus.py
library-oracle --java JAVA --probe PROBE [--library-extra DIR]` compares them for every file in
[`tests/fixtures/library`](../tests/fixtures/library) in LF, CRLF, CR and BOM+CRLF variants; `--library-extra`
adds local documents such as a saved ComPADRE response. It runs on Linux CI after the build.
`LibraryDump.java --generate` writes a collection and a library with OSP's own API; the committed copies in
`tests/fixtures/library/generated` must equal a fresh run, and MoTrack's writers reproduce them byte for byte.
The known-difference list in `corpus.py` holds the ArrayList case, which must keep failing in Tracker 6.3.5.

The fixtures are synthetic. The ComPADRE samples follow the schema of the REST responses fetched on 2026-11-22
with invented records. The real responses and OSP's CAB collections and libraries were compared once locally with
`--library-extra` (91 documents with the fixtures, all equal); they are not committed.

## Deliberate differences

- `resources` stored as an ArrayList loads, as on OSP's tip; Tracker 6.3.5 throws `ClassCastException`. Saving
  such a list unchanged keeps it; changing its membership rewrites it as an array.
- `relativePath` matches whole segments (#265). ComPADRE's default mode: #213 and #266.
- Saves keep HTML paths inside archives and write an explicit Unknown type when needed (above).
- Unknown classes are recognized by name, not by loading Java classes; a known Java class of the wrong type is
  `Malformed`, as the Java cast is.
- `hasExternalHtml` lower-cases the target with ASCII rules; Java uses the default locale. OSP lower-cases only
  the target, so `Ball.TRZ!/info.html` with target `Ball.TRZ` counts as external and is written.
- `typeFromPath` takes the media classification from the caller instead of Tracker's video engine registry. Like
  OSP it classifies a target shorter than four characters, including an empty one, as HTML, so clearing a target
  makes a resource HTML; I6 decides whether its editor keeps that behavior.
