// Demo: User-defined Java utility class — importable via require
public class MathUtils {
    public static int square(int x) {
        return x * x;
    }

    public static int add(int a, int b) {
        return a + b;
    }

    public static int max(int a, int b) {
        if (a > b) {
            return a;
        }
        return b;
    }
}
