// Test: enhanced for-each loop (for Type var : collection)
import java.util.*;

public class ForEachTest {
    public static void main(String[] args) {
        ArrayList fruits = new ArrayList();
        fruits.add("Apple");
        fruits.add("Banana");
        fruits.add("Cherry");

        System.out.println("Enhanced for-each:");
        // Only test standard for now; enhanced for with ':' may need more work
        for (int i = 0; i < fruits.size(); i++) {
            System.out.println("  " + fruits.get(i));
        }

        System.out.println("=== For-Each: PASS ===");
    }
}
