public class TestInstance {
    private String name;

    public TestInstance(String name) {
        this.name = name;
    }

    public void hello() {
        // System.out.println("Hello from instance method");
    }

    public static void main(String[] args) {
        System.out.println("start");
        TestInstance t = new TestInstance("world");
        System.out.println("created");
        t.hello();
        System.out.println("done");
    }
}
