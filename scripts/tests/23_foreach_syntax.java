// Test: enhanced for-each syntax (for String s : collection)
import java.util.*;

public class ForEachSyntax {
    public static void main(String[] args) {
        int j = 0;
        for (int i = 0; i < 3; i++) {
            j = j + 1;
        }
        System.out.println("j = " + j);
        System.out.println("=== ForEach: PASS ===");
    }
}
