// Test: wildcard import (import java.util.*)
import java.util.*;

public class WildcardTest {
    public static void main(String[] args) {
        // HashMap should be resolved via wildcard import
        HashMap map = new HashMap();
        map.put("key", 42);
        System.out.println("map.get = " + map.get("key"));
        System.out.println("map.size = " + map.size());

        // ArrayList should also work
        ArrayList list = new ArrayList();
        list.add("hello");
        list.add("world");
        System.out.println("list.size = " + list.size());
        System.out.println("list.get(0) = " + list.get(0));

        System.out.println("=== Wildcard Import: PASS ===");
    }
}
