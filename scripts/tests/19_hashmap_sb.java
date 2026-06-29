// Test: HashMap and StringBuilder
import java.util.HashMap;

public class HashMapSBTest {
    public static void main(String[] args) {
        HashMap ages = new HashMap();
        ages.put("Alice", 30);
        ages.put("Bob", 25);
        System.out.println("size = " + ages.size());
        System.out.println("Alice = " + ages.get("Alice"));
        System.out.println("=== HashMap: PASS ===");
    }
}
