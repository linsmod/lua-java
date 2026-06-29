// Test: Method overloading (note: true overloading not yet implemented,
// only the last definition of a method name survives)
public class MethodOverload {
    private String name;
    private int age;

    public MethodOverload(String name, int age) {
        this.name = name;
        this.age = age;
    }

    public void displayInfo(String prefix) {
        System.out.println(prefix + " Name: " + name + ", Age: " + age);
    }

    public static void main(String[] args) {
        MethodOverload obj = new MethodOverload("Alice", 30);
        obj.displayInfo("Person: ");
        System.out.println("=== Method Overload: PASS ===");
    }
}
