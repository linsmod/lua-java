// scripts/test.java — test Java→Lua compiler + runtime library

class Calculator {

    public static int add(int a, int b) {
        int result = a + b;
        return result;
    }

    public static int factorial(int n) {
        if (n <= 1) {
            return 1;
        }
        return n * factorial(n - 1);
    }

    public static void main(String[] args) {
        int x = 10;
        int y = 20;

        // Console output via System.out
        System.out.println("=== Java Calculator ===");

        // Direct arithmetic
        System.out.print("10 + 20 = ");
        System.out.println(x + y);

        System.out.print("42 * 7 = ");
        System.out.println(42 * 7);

        System.out.print("100 / 3 = ");
        System.out.println(100 / 3);

        System.out.print("100 % 7 = ");
        System.out.println(100 % 7);

        // Boolean logic
        System.out.print("5 < 10 = ");
        System.out.println(5 < 10);

        System.out.print("3 >= 8 = ");
        System.out.println(3 >= 8);

        System.out.print("true == true: ");
        System.out.println(true == true);
        System.out.print("false == true: ");
        System.out.println(false == true);

        // Negation
        System.out.print("!true = ");
        System.out.println(!true);
        System.out.print("!false = ");
        System.out.println(!false);

        // Negative number
        System.out.print("-5 = ");
        System.out.println(-5);

        // String literal
        String msg = "Java-in-Lua works!";
        System.out.println(msg);

        System.out.println("Done!");
    }

}
