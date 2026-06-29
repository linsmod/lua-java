// Test: Array index read arr[i] and write arr[i] = value
public class ArrayIndexTest {
    public static void main(String[] args) {
        // 1. int array
        int[] nums = {10, 20, 30, 40, 50};
        System.out.println("nums[0] = " + nums[0]);
        System.out.println("nums[2] = " + nums[2]);
        System.out.println("nums[4] = " + nums[4]);

        // 2. sum via index in for loop
        int sum = 0;
        for (int i = 0; i < 5; i++) {
            sum = sum + nums[i];
        }
        System.out.println("sum = " + sum);

        // 3. String array
        String[] names = {"Alice", "Bob", "Charlie"};
        System.out.println("names[0] = " + names[0]);
        System.out.println("names[2] = " + names[2]);

        // 4. assign to array element
        nums[1] = 999;
        System.out.println("nums[1] after assign = " + nums[1]);

        // 5. verify other elements unchanged
        System.out.println("nums[0] still = " + nums[0]);

        System.out.println("=== Array Index: PASS ===");
    }
}
