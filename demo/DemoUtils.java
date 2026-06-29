// DemoUtils.java  — 辅助类：演示静态方法、varargs、枚举
package demo;

public class DemoUtils {
    // 静态常量
    public static final double PI = 3.14159;
    public static final int MAX_SCORE = 100;

    // 静态方法
    public static int add(int a, int b) {
        return a + b;
    }

    // varargs 可变参数
    public static int sum(int... numbers) {
        int total = 0;
        for (int i = 0; i < numbers.length; i++) {
            total = total + numbers[i];
        }
        return total;
    }

    // 演示自增/自减
    public static int doubleAndIncrement(int x) {
        x = x + x;
        x++;
        return x;
    }
}
