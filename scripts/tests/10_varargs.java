// Test: Variable arguments (int...)
public class Varargs {
    public static int sum(int... numbers) {
        int total = 0;
        for (int num : numbers) {
            total += num;
        }
        return total;
    }

    public static void main(String[] args) {
        int total = sum(1, 2, 3, 4, 5);
        System.out.println("Sum of 1..5 = " + total);

        System.out.println("=== Varargs: PASS ===");
    }
}
