// Test: Array declaration, literal init, enhanced for
public class ArrayTest {
    public static void main(String[] args) {
        int[] numbers = {1, 2, 3, 4, 5};
        for (int num : numbers) {
            System.out.print(num + " ");
        }
        System.out.println();

        System.out.println("=== Array: PASS ===");
    }
}
