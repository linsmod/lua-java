// Test: Instance method + this.field access
public class InstanceMethod {
    private String name;
    private int age;

    public InstanceMethod(String name, int age) {
        this.name = name;
        this.age = age;
    }

    public void displayInfo() {
        System.out.println("Name: " + name + ", Age: " + age);
    }

    public static void main(String[] args) {
        InstanceMethod obj = new InstanceMethod("Alice", 30);
        obj.displayInfo();
        System.out.println("=== Instance Method: PASS ===");
    }
}
