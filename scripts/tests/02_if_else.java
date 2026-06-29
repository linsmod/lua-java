// Test: if/else conditional
public class IfElse {
    public static void main(String[] args) {
        int value = 10;

        if (value > 5) {
            System.out.println("value is greater than 5");
        } else {
            System.out.println("value is <= 5");
        }

        if (value < 3) {
            System.out.println("SHOULD NOT PRINT");
        } else {
            System.out.println("value is not < 3");
        }

        System.out.println("=== If/Else: PASS ===");
    }
}
