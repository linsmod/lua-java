// Test: ArrayList with generics
import java.util.ArrayList;
import java.util.List;

public class ArrayListTest {
    public static void main(String[] args) {
        List<String> fruits = new ArrayList<>();
        fruits.add("Apple");
        fruits.add("Banana");
        fruits.add("Cherry");
        for (String fruit : fruits) {
            System.out.println(fruit);
        }

        System.out.println("=== ArrayList: PASS ===");
    }
}
