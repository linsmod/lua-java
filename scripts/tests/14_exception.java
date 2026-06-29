// Test: try/catch/finally, throw, throws
public class ExceptionTest {

    public static int divide(int a, int b) throws ArithmeticException {
        if (b == 0) {
            throw new ArithmeticException("Cannot divide by zero");
        }
        return a / b;
    }

    public static void main(String[] args) {
        // Test 1: normal division
        int r1 = divide(10, 2);
        System.out.println("10 / 2 = " + r1);

        // Test 2: try/catch
        try {
            int r2 = divide(10, 0);
            System.out.println("SHOULD NOT PRINT: " + r2);
        } catch (ArithmeticException e) {
            System.out.println("Exception caught: " + e.getMessage());
        } finally {
            System.out.println("Finally block executed");
        }

        System.out.println("=== Exception: PASS ===");
    }
}
