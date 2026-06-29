// Test: Math operations — arithmetic, compound assignments, comparisons
public class MathOperations {

    public static void main(String[] args) {
        int a = 10;
        int b = 3;
        double x = 7.5;
        double y = 2.0;

        // ---- 1. Basic arithmetic ----
        int add = a + b;
        int sub = a - b;
        int mul = a * b;
        int div = a / b;
        int mod = a % b;

        System.out.println("10 + 3 = " + add);
        System.out.println("10 - 3 = " + sub);
        System.out.println("10 * 3 = " + mul);
        System.out.println("10 / 3 = " + div);
        System.out.println("10 % 3 = " + mod);

        double dadd = x + y;
        double dsub = x - y;
        double dmul = x * y;
        double ddiv = x / y;

        System.out.println("7.5 + 2.0 = " + dadd);
        System.out.println("7.5 - 2.0 = " + dsub);
        System.out.println("7.5 * 2.0 = " + dmul);
        System.out.println("7.5 / 2.0 = " + ddiv);

        // ---- 2. Compound assignments ----
        int c = 10;
        c += 3;
        System.out.println("10 += 3  => " + c);
        c -= 2;
        System.out.println("13 -= 2  => " + c);
        c *= 4;
        System.out.println("11 *= 4  => " + c);
        c /= 5;
        System.out.println("44 /= 5  => " + c);
        c %= 3;
        System.out.println("8 %= 3   => " + c);

        // ---- 3. Increment / decrement ----
        int i = 5;
        i++;
        System.out.println("5++ => " + i);
        i--;
        System.out.println("6-- => " + i);

        // ---- 4. Operator precedence ----
        int prec1 = 2 + 3 * 4;
        int prec2 = (2 + 3) * 4;
        int prec3 = 10 - 3 + 2;
        int prec4 = 10 - (3 + 2);

        System.out.println("2 + 3 * 4 = " + prec1);
        System.out.println("(2 + 3) * 4 = " + prec2);
        System.out.println("10 - 3 + 2 = " + prec3);
        System.out.println("10 - (3 + 2) = " + prec4);

        // ---- 5. Comparison operators ----
        boolean gt = a > b;
        boolean lt = a < b;
        boolean ge = a >= 10;
        boolean le = b <= 3;
        boolean eq = a == 10;
        boolean ne = a != b;

        System.out.println("10 > 3  = " + gt);
        System.out.println("10 < 3  = " + lt);
        System.out.println("10 >= 10 = " + ge);
        System.out.println("3 <= 3  = " + le);
        System.out.println("10 == 10 = " + eq);
        System.out.println("10 != 3 = " + ne);

        // ---- 6. Unary minus ----
        int negA = -a;
        double negX = -x;
        System.out.println("-10 = " + negA);
        System.out.println("-7.5 = " + negX);

        // ---- 7. Mixed expression with multiple operators ----
        int mixed = a * b + a / b - (a % b);
        System.out.println("10*3 + 10/3 - 10%3 = " + mixed);

        System.out.println("=== Math Operations: PASS ===");
    }
}
