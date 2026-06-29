// Test: Static method definition and call
public class StaticMethod {
    public static int add(int a, int b) {
        return a + b;
    }

    public static void main(String[] args) {
        int result = add(5, 7);
        System.out.println("5 + 7 = " + result);

        System.out.println("=== Static Method: PASS ===");
    }
}
