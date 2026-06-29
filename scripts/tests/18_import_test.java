// Test: Import user-defined Java class via require
import com.example.MathUtils;

public class ImportTest {
    public static void main(String[] args) {
        int sq = MathUtils.square(5);
        System.out.println("square(5) = " + sq);

        int sum = MathUtils.add(10, 20);
        System.out.println("add(10, 20) = " + sum);

        int maxVal = MathUtils.max(7, 3);
        System.out.println("max(7, 3) = " + maxVal);

        System.out.println("=== Import: PASS ===");
    }
}
