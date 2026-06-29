// DemoPerson.java  — 辅助类：演示构造函数、实例方法、实例字段
package demo;

public class DemoPerson {
    private String name;
    private int age;

    public DemoPerson(String name, int age) {
        this.name = name;
        this.age = age;
    }

    public String getName() {
        return this.name;
    }

    public int getAge() {
        return this.age;
    }

    public void birthday() {
        this.age = this.age + 1;
    }

    public String describe() {
        return this.name + " (" + this.age + ")";
    }
}
