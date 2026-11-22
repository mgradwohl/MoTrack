import java.math.BigDecimal;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Random;
import java.util.Set;
import java.util.function.ToDoubleFunction;
import org.opensourcephysics.display.OSPRuntime;
import org.opensourcephysics.numerics.SuryonoParser;

/**
 * Kernel oracle for MoTrack's expression engine (K1). Every case is parsed and evaluated with OSP's actual
 * SuryonoParser from the pinned Tracker 6.3.5 JAR, the parser behind Data Builder functions, model and fit
 * functions, parameters and data functions. Doubles are written as Double.toString text plus raw IEEE bits.
 *
 * Cases tagged with a defect also carry independent "intended" values: a bug-free equivalent expression
 * evaluated by the same parser, an fdlibm-style formula, or a direct evaluation of the generating tree.
 * The same deterministic generators run on every invocation, so a rerun reproduces the committed fixture.
 */
public final class ExpressionOracle {
    private static final int RANDOM_SAMPLES = 2000;
    private static final double NAN = Double.NaN;
    private static final double INF = Double.POSITIVE_INFINITY;
    private static final double LN2 = 6.93147180559945286227e-01;

    /** Single-argument function points: signed zeros, rounding ties, range limits and domain errors. */
    private static final double[] POINTS = {0.0, -0.0, 0.5, -0.5, 1.0, -1.0, 1.5, 2.5, -2.5, 3.5, 2.0, 10.0, 100.0, 0.1,
        1e-20, -1e-20, 1e-8, 1e8, -1e8, 1e200, 1e300, -1e300, 0.49999999999999994, 4503599627370495.5, 4503599627370497.0,
        9.223372036854775807E18, -9.3e18, 1e19, Math.PI, Math.PI / 2, Math.E, 0.9999999999999999, 1.0000000000000002,
        710.0, -746.0, Double.MIN_VALUE, Double.MIN_NORMAL, Double.MAX_VALUE, NAN, INF, -INF};
    private static final double[][] PAIRS = {{1, 2}, {2, 1}, {-0.0, 0.0}, {0.0, -0.0}, {NAN, 1}, {1, NAN}, {5, 3},
        {-5, 3}, {5, -3}, {5.5, 2}, {4, 2}, {-4, 2}, {7, 2}, {1, 0}, {0, 0}, {-1, 0}, {INF, 1}, {1, INF}, {-1, -INF},
        {INF, INF}, {-INF, INF}, {3, 1e-300}, {1e300, 1e-300}, {0.5, -0.5}};
    private static final String[] INEXACT = {"sin", "cos", "tan", "ln", "log", "exp", "^"};

    private final List<Case> cases = new ArrayList<>();
    private final Set<String> ids = new HashSet<>();

    private static final class Case {
        final String id;
        final String group;
        final String expression;
        final String[] variables;
        final List<double[]> points = new ArrayList<>();
        String defect;
        String intendedExpression;
        ToDoubleFunction<double[]> intended;
        boolean random;

        Case(String id, String group, String expression, String[] variables) {
            this.id = id;
            this.group = group;
            this.expression = expression;
            this.variables = variables;
        }

        Case at(double... values) {
            if (values.length != variables.length) throw new IllegalArgumentException(id + ": wrong value count");
            points.add(values);
            return this;
        }

        Case defect(String name, String equivalent) {
            defect = name;
            intendedExpression = equivalent;
            return this;
        }

        Case defect(String name, ToDoubleFunction<double[]> evaluation) {
            defect = name;
            intended = evaluation;
            return this;
        }
    }

    public static void main(String[] args) {
        try {
            if (args.length != 1) throw new IllegalArgumentException("Usage: ExpressionOracle output.json");
            if (!OSPRuntime.VERSION.equals("6.3.5")) throw new IllegalStateException("Expected Tracker 6.3.5");
            ExpressionOracle oracle = new ExpressionOracle();
            oracle.curated();
            oracle.functions();
            oracle.literals();
            oracle.generated();
            Files.writeString(Path.of(args[0]), oracle.report(), StandardCharsets.UTF_8);
            System.exit(0);
        } catch (Throwable error) {
            error.printStackTrace();
            System.exit(1);
        }
    }

    private Case add(String id, String group, String expression, String... variables) {
        if (!id.matches("[a-z0-9-]+") || !ids.add(id)) throw new IllegalArgumentException("Invalid or duplicate case " + id);
        Case c = new Case(id, group, expression, variables);
        cases.add(c);
        return c;
    }

    private Case constant(String id, String group, String expression) {
        return add(id, group, expression).at();
    }

    private void constants(String group, String... idsAndExpressions) {
        for (int i = 0; i < idsAndExpressions.length; i += 2) constant(group + "-" + idsAndExpressions[i], group, idsAndExpressions[i + 1]);
    }

    private void curated() {
        constants("literal", "zero", "0", "zero-point", "0.0", "one", "1", "one-point", "1.0", "integer", "42",
            "leading-zeros", "007", "leading-zero-fraction", "000.25", "fraction", "1.5", "leading-dot", ".5",
            "trailing-dot", "5.", "dot", ".", "dot-exponent", ".e5", "exponent", "1e3", "exponent-upper", "1E3",
            "exponent-plus", "1e+3", "exponent-minus", "1e-3", "fraction-exponent", "1.5e-3", "dot-fraction-exponent",
            ".5e1", "trailing-dot-exponent", "1.e2", "missing-exponent", "1e", "missing-exponent-sign", "1e+",
            "missing-negative-exponent", "1e-", "upper-missing-exponent", "2.5E", "late-missing-exponent", "1+1e",
            "double-dot", "1..2", "two-dots", "1.2.3", "hex", "0x10", "overflow", "1e400", "underflow", "1e-400",
            "min-subnormal", "4.9e-324", "below-half-subnormal", "2.4703282292062327e-324",
            "above-half-subnormal", "2.4703282292062328e-324", "max", "1.7976931348623157e308",
            "max-round-down", "1.7976931348623158e308", "max-round-up", "1.7976931348623159e308",
            "normal-boundary", "2.2250738585072011e-308", "normal-boundary-up", "2.2250738585072012e-308",
            "two-pow-53-plus-one", "9007199254740993", "tenth", "0.1", "thirty-digits", "123456789012345678901234567890",
            "ten-pow-23", "1e23", "near-tie", "8.589973e9", "pi-digits", "3.141592653589793238462643383279",
            "underscore", "1_000", "huge-exponent", "1e99999999999999999999", "huge-negative-exponent", "1e-99999999999999999999",
            "zero-huge-exponent", "0e99999", "number-identifier", "2x", "number-exponent-identifier", "1e5e5",
            "long-zeros", "0." + "0".repeat(400) + "1e400", "long-mantissa", "1" + "0".repeat(350) + "e-350",
            "infinity-name", "Infinity", "nan-name", "NaN");
        constants("constant", "pi", "pi", "e", "e", "negative-pi", "-pi", "two-pi", "2*pi", "e-squared", "e^2",
            "upper-pi", "PI", "upper-e", "E", "pie", "pie", "pi-call", "pi(1)", "e-call", "e(1)");

        constants("precedence", "product-before-sum", "1+2*3", "parenthesized-sum", "(1+2)*3", "sum-after-product", "2*3+4",
            "left-division", "8/2/2", "left-subtraction", "1-2-3", "right-subtraction", "1-(2-3)", "power-right",
            "2^3^2", "power-left-parenthesized", "(2^3)^2", "negated-power", "-2^2", "parenthesized-negative-base",
            "(-2)^2", "negated-parenthesized-power", "-(2)^2", "power-product", "2^2*3", "product-power", "2*3^2",
            "sum-product-power", "1+2*3^2", "quotient-product", "10/4*2", "product-quotient", "2*3/4", "third", "1/3",
            "tenths", "0.1+0.2", "product-negation", "2*-3", "triple-minus", "2---3", "parenthesized-negation", "1-(-2)",
            "negation-before-product", "2+-3*4", "negation-before-power", "2+-3^2", "negation-before-quotient", "2--6/3",
            "unary-chain", "-+-3", "unary-plus", "+3", "double-negation", "--3",
            "negated-parenthesized", "-(-3)", "power-products", "2^3*4^2", "trailing-power-sum", "2^2+3",
            "overflow-product", "1e308*10", "negative-overflow-product", "-1e308*10", "negative-zero", "-0",
            "negative-zero-product", "0*-1", "power-of-power-chain", "2^2^2^2");
        add("precedence-model-sum", "precedence", "x0+v*t+a*t^2/2", "x0", "v", "a", "t").at(1, 2, 3, 4).at(0, -1, 9.8, 0.5);
        add("precedence-negated-variable-power", "precedence", "-x^2", "x").at(3).at(-3);
        add("precedence-negated-variable-power-product", "precedence", "-x^2*3", "x").at(2);

        // Known OSP defects: Tracker 6.3.5 applies a negated exponent after the power, and ends a +/- term at
        // a power that is followed by * or /. The intended forms are parenthesized so OSP evaluates them correctly.
        String exponent = "negated-exponent";
        constant("defect-negative-exponent", "defect", "2^-3").defect(exponent, "2^(-3)");
        constant("defect-negative-exponent-chain", "defect", "2^-1^2").defect(exponent, "2^(-(1^2))");
        constant("defect-negated-negative-exponent", "defect", "-2^-2").defect(exponent, "-(2^(-2))");
        constant("defect-negative-zero-exponent", "defect", "2^-0").defect(exponent, "2^(-0)");
        constant("defect-inner-negative-exponent", "defect", "2^3^-1").defect(exponent, "2^(3^(-1))");
        constant("defect-negative-exponent-product", "defect", "2*4^-1").defect(exponent, "2*(4^(-1))");
        constant("defect-negative-zero-base-exponent", "defect", "(-0)^-1").defect(exponent, "(-0)^(-1)");
        add("defect-variable-negative-exponent", "defect", "x^-2", "x").at(2).at(-0.5).at(0).defect(exponent, "x^(-2)");
        add("defect-inverse-square", "defect", "k*q/r^-2", "k", "q", "r").at(2, 3, 4).defect(exponent, "k*q/(r^(-2))");
        String product = "power-product-in-sum";
        constant("defect-power-product-in-sum", "defect", "1+2^2*3").defect(product, "1+(2^2*3)");
        constant("defect-power-quotient-in-difference", "defect", "1-2^2/4").defect(product, "1-(2^2/4)");
        constant("defect-power-product-chain", "defect", "1+2^2*3/4*5").defect(product, "1+(2^2*3/4*5)");
        constant("defect-power-product-then-sum", "defect", "1+2^2*3+4").defect(product, "1+(2^2*3)+4");
        constant("defect-negated-power-product", "defect", "1--2^2*3").defect(product, "1-(-(2^2)*3)");
        add("defect-kinematics", "defect", "x0+t^2*a/2", "x0", "t", "a").at(1, 2, 3).defect(product, "x0+(t^2*a/2)");
        constant("defect-both", "defect", "1+2^-1*4").defect("negated-exponent,power-product-in-sum", "1+(2^(-1)*4)");
        // OSP drops the sign of a +/- operand unless the operand is followed by ^, * or /.
        String negation = "dropped-negation";
        constant("defect-negated-addend", "defect", "2+-3").defect(negation, "2+(-3)");
        constant("defect-negated-subtrahend", "defect", "2--3").defect(negation, "2-(-3)");
        constant("defect-negated-addend-chain", "defect", "1+-2+-3").defect(negation, "1+(-2)+(-3)");
        constant("defect-negated-call-addend", "defect", "1+-sin(1)").defect(negation, "1+(-sin(1))");
        constant("defect-negated-addend-relation", "defect", "1+-2<0").defect(negation, "1+(-2)<0");
        constant("defect-negated-addend-argument", "defect", "max(1+-5,0)").defect(negation, "max(1+(-5),0)");
        add("defect-negated-variable-subtrahend", "defect", "x--y", "x", "y").at(1, 2).at(0, 0).defect(negation, "x-(-y)");
        constant("defect-all-three", "defect", "1+-2+2^-1*4").defect("dropped-negation,negated-exponent,power-product-in-sum", "1+(-2)+(2^(-1)*4)");

        constants("division", "by-zero", "1/0", "negative-by-zero", "-1/0", "zero-by-zero", "0/0",
            "by-negative-zero", "1/-0", "by-cancelled", "1/(1-1)", "large-by-zero", "1e200/0", "zero-quotient-product",
            "(0/0)*1", "by-tiny", "1/1e-300");
        add("division-variable", "division", "1/x", "x").at(0).at(-0.0).at(2).at(NAN).at(INF);
        add("division-numerator", "division", "x/0", "x").at(0).at(-3).at(NAN).at(INF).at(1e300);

        constants("relation", "less", "1<2", "less-false", "2<1", "less-equal", "1<=1", "greater-equal-false", "1>=2",
            "equal", "1=1", "not-equal", "1<>2", "greater", "1>2", "sum-equal", "1+1=2", "less-sum", "1<2+3",
            "parenthesized", "(1<2)", "chained", "1<2<3", "double-equal", "1==1", "equal-less", "1=<2", "equal-greater",
            "1=>2", "greater-less", "1><2", "shift-right", "1>>2", "shift-left", "1<<2", "missing-right", "1<>",
            "missing-left", "<1", "spaced", "1 < 2", "spaced-less-equal", "1 <= 2", "split-less-equal", "1 < = 2",
            "parenthesized-chain", "(1<2)<3", "negated", "-1<0", "negated-relation", "-(1<2)");
        add("relation-variable", "relation", "x<1", "x").at(0).at(1).at(2).at(NAN).at(-INF);
        add("relation-nan-equal", "relation", "x=x", "x").at(1).at(NAN);
        add("relation-nan-not-equal", "relation", "x<>x", "x").at(1).at(NAN);
        add("relation-signed-zero", "relation", "x=y", "x", "y").at(0.0, -0.0);

        constants("boolean", "and", "(1<2)&(2<3)", "or", "(1<2)|(3<2)", "and-false", "(1<2)&(3<2)",
            "right-associative-and-or", "(1<2)&(3<2)|(1<2)", "right-associative-or-and", "(0<1)|(1<0)&(1<0)",
            "and-or-grouping", "(1<0)&(1<0)|(0<1)", "or-and-grouping", "(0<1)|(0<1)&(1<0)",
            "not", "!(1<2)", "not-false", "!(1>2)", "not-number", "!(1)", "not-without-parentheses", "!1",
            "numbers", "1&1", "boolean-sum", "(1<2)+1", "sum-boolean", "1+(1<2)", "boolean-argument", "abs(1<2)",
            "boolean-argument-sum", "abs(1<2)+1", "spaced-not", "! (1<2)", "not-space-operator", "!(1<2) & (1<2)",
            "not-and", "!(1<2)&(1<2)", "unparenthesized-relations", "1<2&2<3", "boolean-power", "(1<2)^2",
            "negated-not", "-!(1<2)", "relation-in-not", "!(1<2<3)", "or-number", "(1<2)|1", "not-comma", "!(1<2,3)",
            "boolean-product", "(1<2)*2", "product-boolean", "2*(1<2)", "power-boolean", "2^(1<2)");
        add("boolean-variable-not", "boolean", "!(x<1)", "x").at(0).at(2).at(NAN);

        constants("if", "true", "if(1<2,3,4)", "false", "if(2<1,3,4)", "spaced", "if (1<2, 3, 4)",
            "missing-else", "if(1<2,3)", "condition-only", "if(1<2)", "no-parenthesis", "if 1", "extra-argument",
            "if(1<2,3,4,5)", "in-sum", "if(1<2,3,4)+1", "in-product", "2*if(2<1,1,2)", "power", "if(1<2,3,4)^2",
            "boolean-branches", "if(1<2,1<2,2<1)", "compound-condition", "if((1<2)&(2<3),1,0)", "empty", "if()",
            "nested-in-condition", "if(if(1<2,1,0),5,6)", "call-name", "if", "missing-condition", "if(,1,2)",
            "unterminated", "if(1<2,1,", "nan-branch", "if(1<2,0/0*sqrt(-1),1)", "relation-in-relation",
            "if(1<if(1<2,2,0),1,0)", "relation-in-relation-call", "1<abs(2<3)", "relation-after-boolean", "if((1<2)&(2<3)<1,1,0)");
        add("if-numeric-condition", "if", "if(x,1,2)", "x").at(0).at(-0.0).at(NAN).at(5);
        add("if-help-example", "if", "if(x<0,x^2,-x^2)", "x").at(-2).at(3).at(0);
        add("if-nested", "if", "if(x<0,if(x<-1,1,2),3)", "x").at(-2).at(-0.5).at(1);
        add("if-random-branch", "if", "if(x<0,random(0),x)", "x").at(-1).at(2);

        constants("syntax", "empty", "", "space", " ", "spaces", "   ", "plus", "+", "minus", "-", "times", "*",
            "trailing-plus", "1+", "trailing-times", "1*", "open", "(", "close", ")", "unclosed", "(1", "extra-close", "1)",
            "double-open", "((1)", "double-close", "(1))", "empty-parentheses", "()", "juxtaposed", "1 2", "comma", "1,2",
            "semicolon", "1;2", "lone-comma", ",", "dotted-name", "a.b", "ampersand", "&", "bar", "|", "trailing-and", "1&",
            "or-numbers", "1|2", "less", "<", "equals", "=", "trailing-equals", "1=", "trailing-power", "2^", "leading-power",
            "^2", "double-power", "2^^3", "double-times", "2**3", "double-divide", "2//3", "factorial", "3!", "bang", "!",
            "number-call", "1(2)", "parenthesized-call", "(1)(2)", "spaced-call", "1 (2)", "tab", "1\t+2",
            "escaped", "1\\+2", "only-escape", "\\", "escaped-name", "p\\i", "trailing-space", "1 ", "leading-space", " 1",
            "inner-spaces", "  1  +  2  ", "space-dot", "1 . 5", "plus-times", "1+*2", "unicode-name", "θ",
            "astral-name", "1+😀", "times-unknown", "2*ω", "null-character", "1+\u0000");
        constants("reference", "definition", "x;a:1", "missing-colon", "x;a", "missing-name", "x;:1", "blank-name",
            "x; :1", "used", "a;a:2", "bad-reference-body", "x;a:1+", "spaced-segment", "x;  a", "two", "x;a:1;b:2",
            "trailing", "1;", "empty-reference-expression", "x;a:", "padded-name", "x; a :1");

        constants("call", "sin-no-parenthesis", "sin x", "sin-unclosed", "sin(1", "sin-empty", "sin()", "sin-two",
            "sin(1,2)", "min-one", "min(1)", "min-three", "min(1,2,3)", "sin-spaced", "sin (1)", "upper-sin", "Sin(1)",
            "all-upper", "SIN(1)", "sin-power", "sin(1)^2", "sqr", "sqr(3)", "no-separator", "sin1", "nested",
            "sin(sin(sin(1)))", "nested-extended", "max(min(1,2),3)", "atan2", "atan2(1,1)", "mod-negative", "mod(7,-3)",
            "ln-e", "ln(e)", "log-thousand", "log(1000)", "log-hundred", "log(100)", "exp-one", "exp(1)", "pow-unknown",
            "pow(2,3)", "sin-bare", "sin", "max-bare", "max", "min-missing-second", "max(1,)", "min-unclosed",
            "min(1,2", "max-spaced", "max ( 1 , 2 )", "comma-in-single", "abs(1,2)", "random-zero", "random(0)",
            "boolean-extended", "max(1<2,2)", "power-argument", "sqrt(2^2)", "negative-argument", "abs(-3)");

        add("variable-sum", "variable", "x+y", "x", "y").at(1, 2).at(-0.0, 0.0).at(NAN, 1);
        add("variable-difference", "variable", "y-x", "x", "y").at(1, 2);
        add("variable-unicode", "variable", "θ*ω", "θ", "ω").at(2, 3);
        add("variable-braces", "variable", "v_{x}*t", "v_{x}", "t").at(2, 3);
        add("variable-primes", "variable", "x'*x@", "x'", "x@").at(2, 3);
        add("variable-duplicate", "variable", "x", "x", "x").at(1, 2);
        add("variable-function-name", "variable", "sin", "sin").at(1);
        add("variable-function-call", "variable", "sin(2)", "sin").at(1);
        add("variable-e", "variable", "e", "e").at(7);
        add("variable-pi", "variable", "pi*2", "pi").at(7);
        add("variable-if", "variable", "if", "if").at(7);
        add("variable-space", "variable", "a b", "a b").at(7);
        add("variable-unknown", "variable", "y", "x").at(1);
        add("variable-digit-start", "variable", "2x", "2x").at(1);
        add("variable-none", "variable", "x").at();
        add("variable-dummy", "variable", "'*@+`-~/#", "'", "@", "`", "~", "#").at(2, 3, 4, 5, 6);
        add("variable-prefix", "variable", "xx", "x").at(1);
        add("variable-space-separated", "variable", "x x", "x").at(1);
        add("variable-bang", "variable", "a!b+1", "a!b").at(2);
        add("variable-trailing-bang", "variable", "x!", "x").at(1);
        add("variable-many", "variable", "m1*m2-k", "m1", "m2", "k").at(2, 3, 4);
        add("variable-special-values", "variable", "x*y", "x", "y").at(INF, 0).at(-INF, -2).at(1e308, 10).at(Double.MIN_VALUE, 0.5);
        add("variable-empty-name", "variable", "x+1", "", "x").at(5, 2);
        add("variable-unicode-error", "variable", "θ+ω", "θ").at(1);

        String sum50 = "1" + "+1".repeat(49);
        add("limit-fifty-ones", "limit", sum50).at();
        add("limit-fifty-one-ones", "limit", sum50 + "+1").at().defect("tracker-code-limit", v -> 51);
        add("limit-variable-sum", "limit", "x" + "+x".repeat(49), "x").at(0.5);
        add("limit-variable-sum-over", "limit", "x" + "+x".repeat(50), "x").at(0.5).defect("tracker-code-limit", v -> 51 * v[0]);
        add("limit-nested-parentheses", "limit", "(".repeat(100) + "1" + ")".repeat(100)).at();
        add("limit-depth-fifty", "limit", "1+(".repeat(49) + "1" + ")".repeat(49)).at();
        add("limit-if-over", "limit", "if(1<2," + sum50 + ",2)").at().defect("tracker-code-limit", v -> 50);
        String sum25 = "1" + "+1".repeat(24);
        add("limit-max-fits", "limit", "max(" + sum25 + "," + sum25 + ")").at();
        add("limit-max-combined-over", "limit", "max(" + sum50 + ",2)").at().defect("tracker-code-limit", v -> 50);
        add("limit-long-literal", "limit", "1" + "0".repeat(300) + "+1").at();
        add("limit-error-after-limit", "limit", sum50 + "+1+*").at();

        // OSP's own parser demo (src/test/ParserTest.java, unchanged on swingJS): new SuryonoParser("0", "x"),
        // then setFunction("sin(x)"), each evaluated at x = 2*Math.PI*i*0.1 for i = 0..10.
        Case demoInitial = add("osp-demo-initial", "osp-demo", "0", "x");
        Case demoSine = add("osp-demo-sine", "osp-demo", "sin(x)", "x");
        for (int i = 0; i <= 10; i++) {
            double x = 2 * Math.PI * i * 0.1;
            demoInitial.at(x);
            demoSine.at(x);
        }

        add("random-scaled", "random", "random(x)", "x").at(0).at(-0.0).at(1).at(-2).at(1e300).at(NAN).at(INF).random = true;
        add("random-offset", "random", "random(1)+1").at().random = true;
    }

    private void functions() {
        String[] names = new SuryonoParser(0).getFunctionNames();
        for (String name : names) {
            if (name.equals("random")) continue;
            boolean extended = name.equals("min") || name.equals("max") || name.equals("mod") || name.equals("atan2");
            Case c = extended ? add("function-" + name, "function", name + "(x,y)", "x", "y") : add("function-" + name, "function", name + "(x)", "x");
            if (extended) {
                for (double[] pair : PAIRS) c.at(pair);
            } else {
                for (double point : POINTS) c.at(point);
            }
            switch (name) {
            case "asinh" -> c.defect("inverse-hyperbolic-precision", v -> asinh(v[0]));
            case "acosh" -> c.defect("inverse-hyperbolic-precision", v -> acosh(v[0]));
            case "atanh" -> c.defect("inverse-hyperbolic-precision", v -> atanh(v[0]));
            default -> { }
            }
        }
    }

    /** Random decimal literals: digit counts, dot placement and exponents across the double range. */
    private void literals() {
        Random random = new Random(20261121L);
        for (int i = 0; i < 120; i++) {
            int whole = random.nextInt(21);
            int fraction = random.nextInt(21);
            if (whole + fraction == 0) whole = 1;
            StringBuilder text = new StringBuilder();
            for (int d = 0; d < whole; d++) text.append((char) ('0' + random.nextInt(10)));
            if (fraction > 0 || random.nextInt(4) == 0) {
                text.append('.');
                for (int d = 0; d < fraction; d++) text.append((char) ('0' + random.nextInt(10)));
            }
            if (whole == 0 && fraction == 0) text.append('0');
            if (random.nextBoolean()) {
                text.append(random.nextBoolean() ? 'e' : 'E');
                int sign = random.nextInt(3);
                if (sign == 1) text.append('+');
                if (sign == 2) text.append('-');
                text.append(random.nextInt(345));
            }
            constant(String.format("generated-literal-%03d", i), "generated-literal", text.toString());
        }
    }

    // ---- Generated arithmetic: rendered with standard precedence, intended value from the generating tree ----

    private abstract static class Node {
        abstract double eval(double[] v, boolean[] exact);

        abstract int precedence();
    }

    private static final int SUM = 1, PRODUCT = 2, NEGATION = 3, POWER = 4, ATOM = 5;

    private static final class Num extends Node {
        final String text;
        final double value;

        Num(String text) {
            this.text = text;
            value = Double.parseDouble(text.startsWith(".") ? "0" + text : text);
        }

        double eval(double[] v, boolean[] exact) { return value; }

        int precedence() { return ATOM; }
    }

    private static final class Var extends Node {
        final int index;

        Var(int index) { this.index = index; }

        double eval(double[] v, boolean[] exact) { return v[index]; }

        int precedence() { return ATOM; }
    }

    private static final class Neg extends Node {
        final Node operand;

        Neg(Node operand) { this.operand = operand; }

        double eval(double[] v, boolean[] exact) { return -operand.eval(v, exact); }

        int precedence() { return NEGATION; }
    }

    private static final class Bin extends Node {
        final char op;
        final Node left;
        final Node right;

        Bin(char op, Node left, Node right) {
            this.op = op;
            this.left = left;
            this.right = right;
        }

        double eval(double[] v, boolean[] exact) {
            double a = left.eval(v, exact), b = right.eval(v, exact);
            switch (op) {
            case '+': return a + b;
            case '-': return a - b;
            case '*': return a * b;
            case '/': return b == 0 ? a / 1.0e-128 : a / b; // OSP's divide-by-zero trap, kept by MoTrack
            default:
                Double power = exactPower(a, b);
                if (power == null) {
                    exact[0] = false;
                    return Math.pow(a, b);
                }
                return power;
            }
        }

        int precedence() { return op == '^' ? POWER : (op == '*' || op == '/') ? PRODUCT : SUM; }
    }

    private static final class Call extends Node {
        final String name;
        final Node[] args;

        Call(String name, Node... args) {
            this.name = name;
            this.args = args;
        }

        double eval(double[] v, boolean[] exact) {
            double a = args[0].eval(v, exact);
            switch (name) {
            case "abs": return Math.abs(a);
            case "sqr": return a * a;
            case "floor": return Math.floor(a);
            case "min": return Math.min(a, args[1].eval(v, exact));
            default: return Math.max(a, args[1].eval(v, exact));
            }
        }

        int precedence() { return ATOM; }
    }

    private static final class If extends Node {
        final Node left;
        final String relation;
        final Node right;
        final Node yes;
        final Node no;

        If(Node left, String relation, Node right, Node yes, Node no) {
            this.left = left;
            this.relation = relation;
            this.right = right;
            this.yes = yes;
            this.no = no;
        }

        double eval(double[] v, boolean[] exact) {
            double a = left.eval(v, exact), b = right.eval(v, exact);
            boolean condition = switch (relation) {
            case "<" -> a < b;
            case ">" -> a > b;
            case "<=" -> a <= b;
            case ">=" -> a >= b;
            case "=" -> a == b;
            default -> a != b;
            };
            return condition ? yes.eval(v, exact) : no.eval(v, exact);
        }

        int precedence() { return ATOM; }
    }

    /**
     * The power when it is exactly representable, computed without Math.pow so case selection does not depend
     * on the JVM's pow intrinsic; null otherwise. Zero, infinite and NaN operands use Math.pow's exactly
     * specified special cases. Every pow implementation returns these exact powers bit for bit.
     */
    private static Double exactPower(double base, double exponent) {
        if (!Double.isFinite(base) || !Double.isFinite(exponent) || base == 0) return Math.pow(base, exponent);
        if (exponent != Math.rint(exponent) || Math.abs(exponent) > 64) return null;
        int k = (int) exponent;
        BigDecimal power = new BigDecimal(base).pow(Math.abs(k));
        double value = power.doubleValue();
        if (!Double.isFinite(value) || new BigDecimal(value).compareTo(power) != 0) return null;
        if (k >= 0) return value;
        double reciprocal = 1.0 / value;
        return new BigDecimal(reciprocal).multiply(power).compareTo(BigDecimal.ONE) == 0 ? reciprocal : null;
    }

    private static final String[] ATOMS = {"1", "2", "3", "4", "0.5", "1.5", "2.5", "0.25", ".5", "3."};
    private static final double[][] GENERATED_POINTS = {{2, -1.5}, {0.5, 3}, {-4, 0.25}, {1, -1}, {0, 2}, {-0.5, 4}};

    private Node atom(Random random) {
        return random.nextInt(3) == 0 ? new Var(random.nextInt(2)) : new Num(ATOMS[random.nextInt(ATOMS.length)]);
    }

    private Node exponentNode(Random random) {
        Node k = new Num(Integer.toString(random.nextInt(4)));
        int kind = random.nextInt(6);
        if (kind == 0) k = new Bin('^', new Num(Integer.toString(1 + random.nextInt(2))), new Num(Integer.toString(random.nextInt(3))));
        return kind >= 3 ? new Neg(k) : k;
    }

    private Node tree(Random random, int depth) {
        int choice = depth <= 0 ? 0 : random.nextInt(12);
        switch (choice) {
        case 0: case 1: return atom(random);
        case 2: return new Neg(tree(random, depth - 1));
        case 3: return new Bin('^', atom(random), exponentNode(random));
        case 4: case 5: return new Bin(random.nextBoolean() ? '+' : '-', tree(random, depth - 1), tree(random, depth - 1));
        case 6: case 7: return new Bin(random.nextBoolean() ? '*' : '/', tree(random, depth - 1), tree(random, depth - 1));
        case 8: return new Bin(random.nextBoolean() ? '+' : '-', tree(random, depth - 1),
            new Bin(random.nextBoolean() ? '*' : '/', new Bin('^', atom(random), exponentNode(random)), tree(random, depth - 1)));
        case 9: {
            String[] names = {"abs", "sqr", "floor", "min", "max"};
            String name = names[random.nextInt(names.length)];
            return name.startsWith("m") ? new Call(name, tree(random, depth - 1), tree(random, depth - 1)) : new Call(name, tree(random, depth - 1));
        }
        case 10: {
            String[] relations = {"<", ">", "<=", ">=", "=", "<>"};
            return new If(tree(random, depth - 1), relations[random.nextInt(relations.length)], tree(random, depth - 1),
                tree(random, depth - 1), tree(random, depth - 1));
        }
        default: return new Bin('^', atom(random), exponentNode(random));
        }
    }

    private static String spaced(Random random, String operator) {
        return random.nextInt(5) == 0 ? " " + operator + " " : operator;
    }

    private static String parenthesize(Random random, Node node) {
        return "(" + render(random, node) + ")";
    }

    private static String render(Random random, Node node) {
        if (node instanceof Num n) return n.text;
        if (node instanceof Var v) return v.index == 0 ? "x" : "y";
        if (node instanceof Neg n) return "-" + (n.operand.precedence() >= NEGATION ? render(random, n.operand) : parenthesize(random, n.operand));
        if (node instanceof Call c) {
            StringBuilder text = new StringBuilder(c.name).append('(');
            for (int i = 0; i < c.args.length; i++) text.append(i == 0 ? "" : spaced(random, ",")).append(render(random, c.args[i]));
            return text.append(')').toString();
        }
        if (node instanceof If f) {
            return "if(" + render(random, f.left) + spaced(random, f.relation) + render(random, f.right) + spaced(random, ",")
                + render(random, f.yes) + spaced(random, ",") + render(random, f.no) + ")";
        }
        Bin b = (Bin) node;
        if (b.op == '^') {
            String base = b.left.precedence() == ATOM ? render(random, b.left) : parenthesize(random, b.left);
            Node r = b.right;
            String exponent = (r.precedence() >= POWER || (r instanceof Neg && random.nextBoolean())) ? render(random, r) : parenthesize(random, r);
            return base + "^" + exponent;
        }
        int p = b.precedence();
        String left = b.left.precedence() >= p ? render(random, b.left) : parenthesize(random, b.left);
        String right = b.right.precedence() > p ? render(random, b.right) : parenthesize(random, b.right);
        return left + spaced(random, String.valueOf(b.op)) + right;
    }

    private void generated() {
        Random random = new Random(1121L);
        int made = 0;
        for (int attempt = 0; made < 160 && attempt < 100000; attempt++) {
            Node tree = tree(random, 1 + random.nextInt(4));
            double[] first = GENERATED_POINTS[random.nextInt(GENERATED_POINTS.length)];
            double[] second = GENERATED_POINTS[random.nextInt(GENERATED_POINTS.length)];
            boolean[] exact = {true};
            double a = tree.eval(first, exact), b = tree.eval(second, exact);
            if (!exact[0] || !Double.isFinite(a) || !Double.isFinite(b)) continue;
            String text = render(random, tree);
            Case c = add(String.format("generated-expression-%03d", made++), "generated-expression", text, "x", "y");
            c.at(first).at(second);
            c.intended = v -> tree.eval(v, new boolean[] {true});
        }
        if (made != 160) throw new IllegalStateException("Expression generator could not create its cases");
    }

    // ---- Independent fdlibm-style inverse hyperbolic functions (OSP uses cancellation-prone formulas) ----

    static double asinh(double x) {
        double t = Math.abs(x), w;
        if (Double.isNaN(x) || Double.isInfinite(x)) return x + x;
        if (t < 0x1p-28) return x;
        if (t > 0x1p28) w = StrictMath.log(t) + LN2;
        else if (t > 2.0) w = StrictMath.log(2.0 * t + 1.0 / (StrictMath.sqrt(x * x + 1.0) + t));
        else {
            double s = x * x;
            w = StrictMath.log1p(t + s / (1.0 + StrictMath.sqrt(1.0 + s)));
        }
        return x > 0 ? w : -w;
    }

    static double acosh(double x) {
        if (Double.isNaN(x) || x < 1.0) return NAN;
        if (x >= 0x1p28) return Double.isInfinite(x) ? x : StrictMath.log(x) + LN2;
        if (x == 1.0) return 0.0;
        if (x > 2.0) return StrictMath.log(2.0 * x - 1.0 / (x + StrictMath.sqrt(x * x - 1.0)));
        double t = x - 1.0;
        return StrictMath.log1p(t + StrictMath.sqrt(2.0 * t + t * t));
    }

    static double atanh(double x) {
        double t = Math.abs(x);
        if (Double.isNaN(x) || t > 1.0) return NAN;
        if (t == 1.0) return x / 0.0;
        if (t < 0x1p-28) return x;
        double r = t < 0.5 ? 0.5 * StrictMath.log1p(t + t + (t + t) * t / (1.0 - t)) : 0.5 * StrictMath.log1p((t + t) / (1.0 - t));
        return x >= 0 ? r : -r;
    }

    // ---- Evaluation and report ----

    private static boolean inexact(String expression) {
        for (String name : INEXACT) if (expression.contains(name)) return true;
        return false;
    }

    private static final class Outcome {
        int code;
        int position;
        String message;
        String exception;
        SuryonoParser parser;
    }

    private static Outcome parse(String expression, String[] variables) {
        Outcome outcome = new Outcome();
        try {
            SuryonoParser parser = new SuryonoParser(variables.length);
            for (int i = 0; i < variables.length; i++) parser.defineVariable(i + 1, variables[i]);
            parser.define(expression);
            parser.parse();
            outcome.code = parser.getErrorCode();
            outcome.position = parser.getErrorPosition();
            outcome.message = parser.getErrorString();
            outcome.parser = parser;
        } catch (Throwable error) {
            outcome.exception = error.getClass().getName();
        }
        return outcome;
    }

    /** Returns the raw value: SuryonoParser substitutes 0 for NaN and records the substitution in a flag. */
    private static double raw(SuryonoParser parser, double[] values) {
        double result = parser.evaluate(values);
        if (parser.evaluatedToNaN()) {
            if (result != 0) throw new IllegalStateException("NaN flag without the documented zero substitution");
            return NAN;
        }
        if (Double.isNaN(result)) throw new IllegalStateException("Unflagged NaN result");
        return result;
    }

    private String report() {
        StringBuilder out = new StringBuilder();
        out.append("{\n  \"schema_version\": 1,\n");
        out.append("  \"reference\": {\"tracker_version\": ").append(json(OSPRuntime.VERSION))
            .append(", \"osp_commit\": \"d56207b0ae737458a31365d368f1c28e4bd3202a\", \"parser\": \"org.opensourcephysics.numerics.SuryonoParser\"},\n");
        out.append("  \"tolerance\": {\"ulp\": 4, \"random_samples\": ").append(RANDOM_SAMPLES).append("},\n");
        out.append("  \"function_names\": [");
        String[] names = new SuryonoParser(0).getFunctionNames();
        for (int i = 0; i < names.length; i++) out.append(i == 0 ? "" : ", ").append(json(names[i]));
        out.append("],\n  \"error_messages\": {");
        for (int code = 0; code <= 15; code++) out.append(code == 0 ? "" : ", ").append('"').append(code).append("\": ").append(json(SuryonoParser.toErrorString(code)));
        out.append("},\n  \"cases\": [\n");
        for (int i = 0; i < cases.size(); i++) {
            out.append(caseJson(cases.get(i))).append(i + 1 < cases.size() ? ",\n" : "\n");
        }
        return out.append("  ]\n}\n").toString();
    }

    private String caseJson(Case c) {
        Outcome outcome = parse(c.expression, c.variables);
        StringBuilder out = new StringBuilder("    {\"id\": ").append(json(c.id)).append(", \"group\": ").append(json(c.group))
            .append(", \"expression\": ").append(json(c.expression)).append(", \"variables\": [");
        for (int i = 0; i < c.variables.length; i++) out.append(i == 0 ? "" : ", ").append(json(c.variables[i]));
        out.append("], \"compare\": ").append(json(c.random ? "random" : inexact(c.expression) ? "ulp" : "exact"))
            .append(", \"defect\": ").append(c.defect == null ? "null" : json(c.defect));
        if (outcome.exception != null) {
            out.append(", \"error\": null, \"exception\": ").append(json(outcome.exception));
        } else if (outcome.code != 0) {
            out.append(", \"error\": {\"code\": ").append(outcome.code).append(", \"position\": ").append(outcome.position)
                .append(", \"message\": ").append(json(outcome.message)).append("}, \"exception\": null");
        } else {
            out.append(", \"error\": null, \"exception\": null");
        }
        SuryonoParser intendedParser = null;
        if (c.intendedExpression != null) {
            Outcome intended = parse(c.intendedExpression, c.variables);
            if (intended.exception != null || intended.code != 0) throw new IllegalStateException(c.id + ": invalid intended form");
            intendedParser = intended.parser;
        }
        out.append(", \"evaluations\": [");
        for (int i = 0; i < c.points.size(); i++) {
            double[] values = c.points.get(i);
            out.append(i == 0 ? "\n      " : ",\n      ").append("{\"values\": [");
            for (int j = 0; j < values.length; j++) out.append(j == 0 ? "" : ", ").append(json(Double.toString(values[j])));
            out.append(']');
            if (outcome.parser != null && outcome.code == 0) {
                if (c.random) {
                    out.append(randomJson(outcome.parser, values));
                } else {
                    double result = raw(outcome.parser, values);
                    out.append(valueJson("result", result));
                    double intended = intendedParser != null ? raw(intendedParser, values) : c.intended != null ? c.intended.applyAsDouble(values) : result;
                    if (Double.doubleToRawLongBits(intended) != Double.doubleToRawLongBits(result) && !(Double.isNaN(intended) && Double.isNaN(result)))
                        out.append(valueJson("intended", intended));
                }
            } else if (outcome.exception != null && c.intended != null) {
                out.append(valueJson("intended", c.intended.applyAsDouble(values)));
            }
            out.append('}');
        }
        return out.append(c.points.isEmpty() ? "]}" : "\n    ]}").toString();
    }

    private static String valueJson(String name, double value) {
        if (!Double.isNaN(value) && Double.doubleToRawLongBits(Double.parseDouble(Double.toString(value))) != Double.doubleToRawLongBits(value))
            throw new IllegalStateException("Double.toString does not round-trip " + value);
        return ", \"" + name + "\": " + json(Double.toString(value)) + ", \"" + name + "_bits\": "
            + json(String.format("%016x", Double.isNaN(value) ? 0x7ff8000000000000L : Double.doubleToRawLongBits(value)));
    }

    /** Checks OSP's random(x) = x * Math.random() over many samples and records its deterministic bounds. */
    private static String randomJson(SuryonoParser parser, double[] values) {
        double scale = values.length == 0 ? 1.0 : values[0];
        double lower = Math.min(0.0, scale), upper = Math.max(0.0, scale);
        boolean nan = Double.isNaN(scale);
        for (int i = 0; i < RANDOM_SAMPLES; i++) {
            double r = raw(parser, values);
            if (values.length == 0) r -= 1.0; // random(1)+1
            if (nan != Double.isNaN(r) || (!nan && (r < lower || r > upper)))
                throw new IllegalStateException("random(x) left its documented range: " + r);
        }
        if (values.length == 0) {
            lower += 1.0;
            upper += 1.0;
        }
        return ", \"nan\": " + nan + ", \"range\": [" + json(Double.toString(lower)) + ", " + json(Double.toString(upper)) + "]";
    }

    static String json(String value) {
        StringBuilder result = new StringBuilder("\"");
        for (char c : value.toCharArray()) {
            if (c == '"' || c == '\\') result.append('\\').append(c);
            else if (c < 32 || c > 126) result.append(String.format("\\u%04x", (int) c));
            else result.append(c);
        }
        return result.append('"').toString();
    }
}
