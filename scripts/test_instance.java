public class TestInstance {
    private String name;

    public TestInstance(String name) {
        this.name = name;
    }

    public void hello() {
        System.out.println("Hello from instance method");
    }

    public static void main(String[] args) {
        TestInstance t = new TestInstance("world");
        t.hello();
    }
}
