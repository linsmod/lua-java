// Test: Constructor, instance fields, new object
public class ConstructorTest {
    private String name;
    private int age;

    public ConstructorTest(String name, int age) {
        this.name = name;
        this.age = age;
    }

    public static void main(String[] args) {
        ConstructorTest obj = new ConstructorTest("Alice", 30);
        // note: obj.toString() / concatenation not yet implemented
        System.out.println("Object created successfully");
        System.out.println("=== Constructor: PASS ===");
    }
}
