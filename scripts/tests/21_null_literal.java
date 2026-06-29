// Test: null literal
public class NullTest {
    public static String getNull() {
        return null;
    }

    public static void main(String[] args) {
        String s = null;
        System.out.println("s == null: " + (s == null));
        System.out.println("s != null: " + (s != null));
        String r = getNull();
        System.out.println("r == null: " + (r == null));
        System.out.println("=== Null Literal: PASS ===");
    }
}
