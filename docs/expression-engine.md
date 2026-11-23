# Expression engine

`motrack_expression` (`src/science/expression.hpp`) compiles and evaluates the mathematical expressions
Tracker stores in experiments: Data Builder data functions and parameters, analytic and dynamic model
functions, initial values and fit-builder functions. It is a Qt-free kernel (slice K1 of the
[full-port plan](migration-plan.md)); binding it to documents, dependency graphs and editors belongs to later
slices. Every one of those Tracker features parses with OSP's `org.opensourcephysics.numerics.SuryonoParser`,
reached through `ParsedMultiVarFunction`, `UserFunction`, `DataFunction` and `Parameter`. Its source is identical
in Tracker 6.3.5 (OSP `d56207b`) and on the current OSP `swingJS` branch (`d49c6ec`, 2026-11-20); the wrapper
classes differ there only by added `hashCode` methods and a JavaScript clipboard branch.

## Using the kernel

```cpp
const std::array<std::string_view, 2> variables{"t", "vx"};
const auto expression = motrack::science::parseExpression("vx*t+if(t<0, 0, t^2)", variables);
const double value = expression ? expression->evaluate(std::array{0.5, 2.0}) // t = 0.5, vx = 2
                                 : std::numeric_limits<double>::quiet_NaN(); // see expression.error()
```

- `Expression` is immutable; copies share one compiled program, and only `parseExpression` creates one. Each call
  compiles independently, so OSP's stale-buffer reuse defect ([#201](https://github.com/mgradwohl/MoTrack/issues/201), [osp#17](https://github.com/OpenSourcePhysics/osp/issues/17)) cannot occur. `evaluate` is
  `const`, allocation-free unless the evaluation stack exceeds 64 entries, and safe to call concurrently. A program that calls `random` draws from a
  per-thread generator, or from a caller-supplied `RandomSource` for reproducible tests and recorded sequences.
- Values are passed in declaration order. A wrong value count returns NaN.
- Introspection serves dependency graphs and caching: `referencedVariables()` (including variables that occur
  only in an untaken `if` branch), `functions()`, `usesRandom()`, `trackerCodeSize()`/`fitsTrackerLimits()`,
  `trackerDifferences()` and `trackerText()` for export.
- `ExpressionOptions` bounds the input to 4096 UTF-16 code units and 128 nesting levels (parentheses, calls,
  relation and boolean operands, power chains) so hostile files cannot exhaust the stack. The bounds apply in both
  dialects, although Tracker itself accepts deeper nesting.

## Grammar and tokens

The compiler is a structural port of SuryonoParser's recursive descent, including its global boolean and
relation flags, so it accepts, rejects and positions errors exactly as Tracker does.

- **Preparation.** Every backslash is removed. The text is parsed as UTF-16 with a closing `)` appended.
  Positions are OSP's one-based UTF-16 positions in the backslash-free text; OSP usually reports the character
  after the offending token, and 0 for an empty expression.
- **Numbers** are `digits[.digits][(e|E)[+|-]digits]` or `.digits…`; a leading dot means `0.`, so `.` and `.e5`
  are 0. The text is rounded like `Double.parseDouble` (exactly, ties to even, any length, overflow to infinity),
  so `1e+5` is 100000 in both dialects; Tracker's models and fits reject it
  ([below](#dialects-and-deliberate-differences)). `1e`, `1e+`, `2.5E` and the split `1e +5` are errors. There
  is no sign in a literal, no hexadecimal form and no implicit multiplication: `2x`, `2pi` and `1 2` are
  "operator expected".
- **Identifiers** are runs of any characters except space and `+ - * / ^ ( ) , < > = & |`, so `v_{x}`, `x'`,
  `θ` and `a!b` are names, while a tab or `.` inside a name is part of it. Matching is exact and case-sensitive:
  `Sin`, `PI` and `E` are unknown. Lookup order is `pi`, `e`, `if`, the built-in functions, then the declared
  variables (first duplicate wins). A variable named like a reserved word (`isReservedName`) can never be
  referenced. Spaces are skipped around operators and operands and between a function name and `(`.
- **Constants**: `pi` = `Math.PI`, `e` = `Math.E`.

| Precedence (high to low) | Operators | Notes |
| --- | --- | --- |
| 1 | `^` | Right-associative: `2^3^2` = 512. Binds tighter than a leading sign: `-2^2` = −4. |
| 2 | unary `+`, `-` | Any number of signs; `--3` = 3. |
| 3 | `*`, `/` | Left-associative. |
| 4 | binary `+`, `-` | Left-associative. |
| 5 | `=`, `<>`, `<`, `>`, `<=`, `>=` | The right operand extends to the next `,` or `)`. Not chainable (`1<2<3` is "invalid operator"), not nestable inside another relation's operand, and not allowed as arithmetic operands (`(1<2)+1` is "invalid operand type"). |
| 6 | `&`, `\|`, prefix `!(…)` | Operands must be relations or boolean results, so `1<2&2<3` needs parentheses. `&` and `\|` share one precedence and group to the right: `A&B\|C` is `A&(B\|C)`. `!` needs parentheses and is not followed by a space skip, so `!(a<b) & c` is an error while `!(a<b)&c` is not; MoTrack keeps this so it never saves text Tracker rejects ([#202](https://github.com/mgradwohl/MoTrack/issues/202)). |

`if(condition, a, b)` evaluates only the selected branch. The condition is any value; zero (either sign) selects
`b` and NaN selects `a`. Relations and boolean operators yield 1 or 0; NaN compares false except with `<>`.

## Functions

All angles are radians: OSP's degree mode exists but no Tracker or OSP tool enables it.

| Function | Definition |
| --- | --- |
| `sin cos tan asin acos atan sinh cosh tanh exp ln sqrt` | The corresponding `Math` function. |
| `log(x)` | Common logarithm: `std::log10` (corrected dialect, exact for powers of ten); see the differences below. |
| `abs ceil floor` | `Math` functions. |
| `int(x)` | `Math.rint`: nearest integer, ties to even (`int(2.5)` = 2, `int(2.7)` = 3). |
| `frac(x)` | `x - rint(x)`, in [−0.5, 0.5]. |
| `round(x)` | `Math.round`: ties toward +∞, NaN → 0, saturated to ±2^63. |
| `sqr(x)` | `x*x`. |
| `sign(x)` | `Math.signum` (signed zeros and NaN pass through). |
| `step(x)` | 0 for x < 0, otherwise 1 (so `step(0)` = `step(NaN)` = 1). |
| `random(x)` | `x * U`, U uniform in [0, 1). |
| `min max` | `Math.min`/`Math.max`: NaN-propagating, −0 < +0. |
| `mod(a, b)` | `Math.IEEEremainder`: the quotient is rounded to even, so `mod(5,3)` = −1. |
| `atan2(y, x)` | `Math.atan2`. |
| `asinh acosh atanh` | Accurate library functions (corrected dialect); see the differences below. |

Division follows OSP's trap in both dialects: a zero divisor is replaced by 1e−128, so `1/0` ≈ 1e128 and
`0/0` = 0. Tracker models rely on this, for example a friction force `-k*vx/sqrt(vx^2+vy^2)` at rest. For data
functions it can turn a missing value into a plausible 0; whether C8 keeps the trap there, marks such rows or
evaluates them with IEEE division is a binding decision to make with that slice.
Evaluation otherwise follows IEEE 754 and returns NaN and infinities as computed. **OSP's evaluate()
substitutes 0 for a NaN result and sets a flag; the kernel never substitutes.** `DataFunction` turns the flag
back into NaN, while `Parameter` values and `UserFunction.evaluate` (models, fits) keep the 0; binding layers
must make that choice explicitly.

## Dialects and deliberate differences

`ExpressionDialect::Tracker` reproduces SuryonoParser's values, errors and positions, with these interface
differences: NaN is returned where OSP returns 0 and sets a flag, a wrong value count returns NaN, the 99-code
overflow is the `TrackerCodeLimit` error instead of an exception, and the length and nesting bounds apply. The
application uses `ExpressionDialect::Corrected` (the default). The kernel oracle found three precedence defects
in OSP's parser, reported upstream as [osp#12](https://github.com/OpenSourcePhysics/osp/issues/12). They contradict both standard notation and OSP's own handling of
the same operators elsewhere, so Tracker computes wrong values whenever they occur:

| Construct | Tracker 6.3.5 | Corrected | `TrackerDifferences` |
| --- | --- | --- | --- |
| Negated exponent ([#185](https://github.com/mgradwohl/MoTrack/issues/185)) | `2^-3` = −8, `x^-2` = −x² | 0.125, 1/x² | `negatedExponent` |
| Power followed by `*` or `/` in a `+`/`-` term ([#186](https://github.com/mgradwohl/MoTrack/issues/186)) | `1+2^2*3` = 15, `x0+t^2*a/2` = (x0+t²)·a/2 | 13 | `powerProductInSum` |
| Sign of a `+`/`-` operand not followed by `^ * /` ([#187](https://github.com/mgradwohl/MoTrack/issues/187)) | `2+-3` = 5, `x--y` = x−y | −1, x+y | `droppedNegation` |
| `log` ([#200](https://github.com/mgradwohl/MoTrack/issues/200), [osp#16](https://github.com/OpenSourcePhysics/osp/issues/16)) | `ln(x)/ln(10)`: `log(1000)` = 2.9999999999999996, `floor(log(1e6))` = 5 | `std::log10`: 3, 6 | `commonLogarithm` |
| `asinh`, `acosh`, `atanh` ([#188](https://github.com/mgradwohl/MoTrack/issues/188), [osp#13](https://github.com/OpenSourcePhysics/osp/issues/13)) | `log(x+sqrt(x*x±1))`, `log((1+x)/(1−x))/2`: `asinh(1e-20)` = 0, `asinh(-1e8)` = −∞, `acosh(1e200)` = ∞ | `std::asinh` etc., within 4 ulp of fdlibm | `inverseHyperbolic` |

Of 160 generated arithmetic expressions in the oracle, 48 evaluate differently in Tracker. Each construct is
detected while parsing in either dialect, so later slices can warn when an opened Tracker experiment contains
one; Tracker itself will compute the old values from the same text. Parenthesized forms such as `2^(-3)` and
`1+(2^2*3)` evaluate identically in both.

A literal with a `+` exponent sign is a deliberate difference of another kind
([#215](https://github.com/mgradwohl/MoTrack/issues/215), [osp#19](https://github.com/OpenSourcePhysics/osp/issues/19)).
SuryonoParser, and with it Tracker's data functions and parameters (`DataFunction`, `Parameter`), evaluate
`1e+5` as 100000. Tracker's analytic and dynamic models and its Fit Builder functions go through `UserFunction`,
whose `padNames` turns `x*1e+5` into `x*1e +5`, a syntax error. `setExpression` then returns false: the function
becomes `0` and evaluates to 0 wherever the model or fit uses it. The saved file keeps the typed text
(`getInputString()`), so Tracker experiments can contain such an expression. This affects every literal with a
`+` sign (`1E+5`, `2.5e+3`, `1.e+2`, `.5e+1`, `1e+0`), even inside `if`. It does not affect `1e5`, `1e-5`, the
constant `e` (`x*e+5`) or parameters named `e` or `E`. Neither dialect reproduces the rejection: like the Data
Tool's text substitution ([#240](https://github.com/mgradwohl/MoTrack/issues/240)), it is a defect in how OSP
prepares a function's text, not in the parser. MoTrack evaluates the literal as written in every context, and
`trackerDifferences().plusExponentSign` marks where Tracker's models and fits compute 0 instead.

The defects also apply in the export direction: a corrected expression such as `x^-2` or `2--3` written to a
`.trk` file as typed computes the old value when Tracker reopens it. `trackerText()` renders the compiled
operations fully parenthesized, with every sign around its operand (`(x^(-2))`, `(2-(-3))`, `(-(!(c)))`) and
literals without a `+` exponent sign (`1e5`: `UserFunction` space-pads names and turns `1e+5` into a parse error
that silently becomes `0`, [#215](https://github.com/mgradwohl/MoTrack/issues/215),
[osp#19](https://github.com/OpenSourcePhysics/osp/issues/19)), and Tracker parses
that text into the same operations. `ExpressionReference.TrackerTextKeepsCorrectedValuesInTracker` checks this for
every oracle case and `Expression.TrackerTextRoundTripsRandomExpressions` for 4000 seeded random expressions, both
through the validated Tracker dialect. Only `log`, `asinh`, `acosh` and `atanh` keep Tracker's
own implementations, so those expressions still need a warning, and the explicit negations can push a near-limit
expression past 99 codes (`trackerCodeSize()` counts what Tracker compiles for the original text; parse the
rendering with the Tracker dialect to check it). The binding rule for C5/C8/I4/I5 (#172, #173): store the
user's text, write `trackerText()` to Tracker files whenever `trackerDifferences()` reports a precedence
construct or `plusExponentSign`, and warn when an unavoidable function difference or the code limit
remains.

Deliberately kept: the division trap, `int` as `rint`, `mod` as IEEE remainder, the boolean grouping and
relation restrictions, and identifier shadowing. These are consistent OSP rules that saved
experiments may depend on, not inconsistencies.

OSP also declares named sub-expressions (`expr;name:definition`), but its split loop keeps the `;` it split on,
so every expression containing `;` fails with "bad reference definition"; MoTrack reports the same error in
both dialects ([#189](https://github.com/mgradwohl/MoTrack/issues/189), [osp#14](https://github.com/OpenSourcePhysics/osp/issues/14)). Tracker stores compiled code in an `int[100]`, so more than 99 postfix codes (about 50 operands)
throw `ArrayIndexOutOfBoundsException` out of the parser ([#190](https://github.com/mgradwohl/MoTrack/issues/190), [osp#15](https://github.com/OpenSourcePhysics/osp/issues/15)). The Tracker dialect reports `TrackerCodeLimit` at the
same point; the corrected dialect accepts longer expressions and `fitsTrackerLimits()` tells an editor that
Tracker could not open the result. OSP's 200-constant and 50-entry stack limits are unreachable within that
code limit.

## Java and C++ numerics

`science/java_math.hpp` (the `motrack_java_math` library, shared with the ODE and analysis kernels) implements the `Math` special cases that differ from `<cmath>`: `pow` returns NaN for
`pow(1, NaN)` and `pow(±1, ±∞)`, `round`, `rint`, `signum`, `min`, `max` and `IEEEremainder`.
`science/decimal_conversion.hpp` converts decimal text with fixed-size big-integer arithmetic instead of
`strtod`, which follows the process locale that Qt applications set. Expression code is compiled with
`FP_CONTRACT OFF` because Java never fuses `a*b+c`.

IEEE arithmetic, square roots, exact functions and literals match Java bit for bit, including signed zeros.
Java specifies `Math.sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `exp`, `log`, `log10` and `pow` to within 1 ulp,
`atan2` to within 2 ulp and `sinh`, `cosh` and `tanh` to within 2.5 ulp; HotSpot implements several with
CPU-specific intrinsics, and each C++ library has its own implementation. Cases that use them, or `^`, compare
within the fixture's declared bound of 4 ulp; none use cancellation-prone compositions.

## Kernel oracle

[`tools/reference/ExpressionOracle.java`](../tools/reference/ExpressionOracle.java) runs headless against the
pinned Tracker JAR and writes [`tests/science/expression-reference.json`](../tests/science/expression-reference.json):
663 cases with 1975 evaluations, including 141 parse errors with OSP codes, messages and positions, and 5 code-limit
exceptions. They cover literals and rounding boundaries, constants, precedence and associativity, unary signs,
division by zero, relations, booleans, `if`, syntax errors, references, calls, variable naming, limits and
`random`, every function at 41 points (or 24 argument pairs) including signed zeros, ties, range limits, NaN and
infinities, OSP's own `src/test/ParserTest.java` demo, 120 seeded random literals and 160 seeded random
expressions. Known-defect cases record an independent intended value: a parenthesized equivalent evaluated by the same parser, an fdlibm-style formula, or
a direct evaluation of the generating tree. Generated expressions only use `^` where the power is exactly
representable, so every `pow` returns the same bits; like other `^` cases they are still compared within the ulp
bound. The oracle source is ASCII-only (`\u` escapes), so the JDK source launcher reads it under any default
charset.

A separate `user_functions` list runs 19 model and fit functions through OSP's `UserFunction` with independent
variables and parameters. Each entry records whether `setExpression` accepted the text, the stored and saved
expression, and the value. 12 of them have a `+` exponent; each records Tracker's 0 and, as the intended value,
the same function written without the sign. The rest are controls (`1e5`, `1e-5`, the constant `e`, parameters
named `e` and `E`). `ExpressionReference.UserFunctionRejectsPlusExponents` checks that `plusExponentSign` predicts
every rejection, that both dialects compute the intended value, and that `trackerText()` removes the sign.

`ExpressionReference.*` in `motrack_tests` checks both dialects against the fixture without Java: the Tracker
dialect against Java's result, the corrected dialect against the intended value where one is recorded, and every
annotated defect against `trackerDifferences()`. Regenerate and diff with
`python tools/corpus.py expression-oracle --java <jdk>/bin/java` (Linux CI runs it); `--record` rewrites the
fixture for review. The diff applies the same exact/ulp rules and keeps its JVM home in a temporary directory.

## Binding for later slices

These rules come from the Tracker classes that wrap the parser; the kernel leaves them to callers.

- **Data functions** (`DataFunction`, slice C8) parse the raw expression over the first dataset's x column, every
  other y column (including other data functions) and the `DatasetManager` constants, in that order. Column names
  lose their TeX subscripting first, so `v_{x}` is referenced as `vx`. An empty expression becomes `0`; a failed
  parse keeps the input text and yields NaN, as does a NaN result.
- **Parameters** (`Parameter`, Data/Model Builders and initial values) parse over the other parameters. Unless the
  text contains `if`, `atan2`, `min`, `max` or `mod`, every `,` is first replaced by `.` to accept locale decimal
  commas (see [#66](https://github.com/mgradwohl/MoTrack/issues/66) for locale-safe decimal input). A parse failure gives NaN, which editors mark invalid, while a
  NaN result becomes 0.
- **Model and fit functions** (`UserFunction`, slices C5 and I4) remove all spaces, pad ASCII names
  (`[A-Za-z_θω]\w*`) with spaces, and replace each independent variable with a dummy name (`'`, `@`, `` ` ``,
  `~`, `#`). Variables are then the independent variables, the parameters and the referenced functions, in that
  order. Analytic models use `t`; dynamic Cartesian forces use `x, vx, y, vy, t`; polar forces use
  `r, vr, θ, ω, t`. The padding splits names containing braces, so models cannot reference them. `evaluate`
  keeps OSP's 0 for NaN; `DatasetCurveFitter` checks the NaN flag. `setExpression` reports an expression
  containing `=` as invalid even though it parses. The padding also rejects every expression with
  `plusExponentSign`. Accept typed `1e+5`. When opening a Tracker model or fit function with
  `plusExponentSign`, compute the written value and warn that Tracker showed the function as invalid and
  computed 0; never substitute that 0. A binding that must reproduce Tracker's own output, such as a corpus
  comparison, treats the flag as UserFunction's failed parse.
- **Names.** Editors reject duplicates, names starting with a digit or parsing as a number, names containing
  `! , . + - * / ^ = > < & | ( )` or spaces, the reserved parser names and the dummy names.
- **Dependencies** come from `referencedVariables()`; Tracker's editors find references by whole-word search in
  the padded text and reject cycles. Results of `usesRandom()` expressions must not be cached.
- **Opening Tracker experiments** should surface `trackerDifferences()` and `fitsTrackerLimits()` to the user
  instead of silently changing the meaning of a saved expression.
