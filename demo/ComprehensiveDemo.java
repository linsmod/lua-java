// ================================================================
// 综合 Demo  — 展示 Java→Lua 编译器支持的所有主要特性
// ================================================================

import java.util.*;           // 通配符导入：ArrayList, HashMap
import java.lang.StringBuilder;
import demo.DemoPerson;       // 自定义类导入
import demo.DemoUtils;        // 自定义类导入（静态方法 + varargs）

public class ComprehensiveDemo {

    // ---- 枚举 ----
    public enum Status {
        PENDING,
        ACTIVE,
        DONE
    }

    // ---- 静态常量 ----
    public static final String TITLE = "=== Comprehensive Demo ===";

    // ---- 静态方法 ----
    public static int factorial(int n) {
        if (n <= 1) {
            return 1;
        }
        return n * factorial(n - 1);
    }

    public static void main(String[] args) {
        // =============================================================
        //  1. 基本类型 & 变量
        // =============================================================
        System.out.println(TITLE);
        System.out.println("");

        int a = 42;
        double pi = 3.14159;
        boolean flag = true;
        String msg = "Hello";
        char ch = 'A';
        int hex = 0xFF;       // 十六进制

        System.out.println("int    = " + a);
        System.out.println("double = " + pi);
        System.out.println("bool   = " + flag);
        System.out.println("String = " + msg);
        System.out.println("char   = " + ch);
        System.out.println("hex    = " + hex);

        // ---- null 字面量 ----
        String nothing = null;
        System.out.print("null check: ");
        if (nothing == null) {
            System.out.println("nothing is null");
        } else {
            System.out.println("nothing is NOT null");
        }

        // =============================================================
        //  2. 运算符 & 表达式
        // =============================================================
        System.out.println("");
        System.out.println("--- Operators ---");

        int x = 10;
        int y = 3;
        System.out.println("10 + 3   = " + (x + y));
        System.out.println("10 - 3   = " + (x - y));
        System.out.println("10 * 3   = " + (x * y));
        System.out.println("10 / 3   = " + (x / y));
        System.out.println("10 % 3   = " + (x % y));

        boolean b1 = true;
        boolean b2 = false;
        System.out.println("true && false = " + (b1 && b2));
        System.out.println("true || false = " + (b1 || b2));
        System.out.println("!true         = " + (!b1));
        System.out.println("10 > 3        = " + (x > y));
        System.out.println("10 == 3       = " + (x == y));
        System.out.println("10 != 3       = " + (x != y));

        // 复合赋值
        int z = 5;
        z += 3;
        System.out.println("5 += 3  = " + z);
        z++;
        System.out.println("z++     = " + z);

        // =============================================================
        //  3. 控制流: if / else if / else
        // =============================================================
        System.out.println("");
        System.out.println("--- If/Else ---");

        int score = 85;
        if (score >= 90) {
            System.out.println("Grade: A");
        } else if (score >= 80) {
            System.out.println("Grade: B");
        } else if (score >= 70) {
            System.out.println("Grade: C");
        } else {
            System.out.println("Grade: F");
        }

        // =============================================================
        //  4. 控制流: switch / case
        // =============================================================
        System.out.println("");
        System.out.println("--- Switch ---");

        int day = 3;
        switch (day) {
            case 1:  System.out.println("Monday");    break;
            case 2:  System.out.println("Tuesday");   break;
            case 3:  System.out.println("Wednesday"); break;
            case 4:  System.out.println("Thursday");  break;
            case 5:  System.out.println("Friday");    break;
            default: System.out.println("Weekend");
        }

        // =============================================================
        //  5. 控制流: for 循环 (标准)
        // =============================================================
        System.out.println("");
        System.out.println("--- For Loop ---");

        int sum_10 = 0;
        for (int i = 1; i <= 10; i++) {
            sum_10 = sum_10 + i;
        }
        System.out.println("Sum 1..10 = " + sum_10);

        // =============================================================
        //  6. 控制流: while 循环
        // =============================================================
        System.out.println("");
        System.out.println("--- While Loop ---");

        int countdown = 5;
        while (countdown > 0) {
            System.out.print(countdown + " ");
            countdown--;
        }
        System.out.println("Go!");

        // =============================================================
        //  7. 控制流: do-while 循环
        // =============================================================
        System.out.println("");
        System.out.println("--- Do-While ---");

        int dw = 0;
        do {
            dw = dw + 10;
        } while (dw < 30);
        System.out.println("do-while result: " + dw);

        // =============================================================
        //  8. 数组
        // =============================================================
        System.out.println("");
        System.out.println("--- Array ---");

        int[] arr = {10, 20, 30, 40, 50};
        int arr_sum = 0;
        for (int i = 0; i < 5; i++) {
            arr_sum = arr_sum + arr[i];
        }
        System.out.println("Array sum = " + arr_sum);

        // =============================================================
        //  9. ArrayList (泛型 new)
        // =============================================================
        System.out.println("");
        System.out.println("--- ArrayList ---");

        ArrayList fruits = new ArrayList();
        fruits.add("Apple");
        fruits.add("Banana");
        fruits.add("Cherry");
        fruits.add("Date");

        System.out.println("size: " + fruits.size());
        System.out.println("fruits[1] = " + fruits.get(1));
        System.out.println("fruits[3] = " + fruits.get(3));

        // =============================================================
        //  10. 增强 for-each 循环
        // =============================================================
        System.out.println("");
        System.out.println("--- For-Each ---");

        for (int i = 0; i < fruits.size(); i++) {
            System.out.print(fruits.get(i) + " ");
        }
        System.out.println("");

        // =============================================================
        //  11. HashMap
        // =============================================================
        System.out.println("");
        System.out.println("--- HashMap ---");

        HashMap ages = new HashMap();
        ages.put("Alice", 30);
        ages.put("Bob", 25);
        ages.put("Charlie", 35);

        System.out.println("Alice   = " + ages.get("Alice"));
        System.out.println("Bob     = " + ages.get("Bob"));
        System.out.println("Charlie = " + ages.get("Charlie"));
        System.out.println("size    = " + ages.size());
        System.out.println("has Bob? " + ages.containsKey("Bob"));
        System.out.println("has Eve? " + ages.containsKey("Eve"));

        // =============================================================
        //  12. StringBuilder (链式调用)
        // =============================================================
        System.out.println("");
        System.out.println("--- StringBuilder ---");

        StringBuilder sb = new StringBuilder();
        sb.append("Hello");
        sb.append(", ");
        sb.append("World");
        sb.append("!");
        System.out.println("SB toString: " + sb.toString());

        // =============================================================
        //  13. 枚举
        // =============================================================
        System.out.println("");
        System.out.println("--- Enum ---");

        int s_pending = Status.PENDING;
        int s_active  = Status.ACTIVE;
        int s_done    = Status.DONE;
        System.out.println("PENDING = " + s_pending);
        System.out.println("ACTIVE  = " + s_active);
        System.out.println("DONE    = " + s_done);

        // =============================================================
        //  14. 静态方法调用 (自定义类)
        // =============================================================
        System.out.println("");
        System.out.println("--- Static Methods ---");

        int add_result = DemoUtils.add(15, 27);
        System.out.println("15 + 27 = " + add_result);

        System.out.println("PI = " + DemoUtils.PI);
        System.out.println("MAX_SCORE = " + DemoUtils.MAX_SCORE);

        // varargs
        int s1 = DemoUtils.sum(1, 2, 3);
        int s2 = DemoUtils.sum(10, 20, 30, 40, 50);
        System.out.println("sum(1,2,3) = " + s1);
        System.out.println("sum(10,20,30,40,50) = " + s2);

        // 自增
        int di = DemoUtils.doubleAndIncrement(5);
        System.out.println("doubleAndIncrement(5) = " + di);

        // =============================================================
        //  15. 递归静态方法
        // =============================================================
        System.out.println("");
        System.out.println("--- Recursion ---");

        int fact5 = factorial(5);
        System.out.println("5! = " + fact5);
        System.out.println("10! = " + factorial(10));

        // =============================================================
        //  16. 对象创建 & 实例方法
        // =============================================================
        System.out.println("");
        System.out.println("--- Objects ---");

        DemoPerson alice = new DemoPerson("Alice", 30);
        DemoPerson bob   = new DemoPerson("Bob",   25);

        System.out.println(alice.describe());
        System.out.println(bob.describe());

        alice.birthday();
        System.out.println("after birthday: " + alice.describe());

        System.out.println("Alice age = " + alice.getAge());
        System.out.println("Bob name  = " + bob.getName());

        // =============================================================
        //  17. 数学运算
        // =============================================================
        System.out.println("");
        System.out.println("--- Math ---");

        System.out.println("ceil(3.14)  = " + Math.floor(3.14));
        System.out.println("floor(3.14) = " + Math.ceil(3.14));
        System.out.println("abs(-42)    = " + Math.abs(-42));
        System.out.println("max(10,20)  = " + Math.max(10, 20));

        // =============================================================
        //  18. 综合：用所有特性计算结果
        // =============================================================
        System.out.println("");
        System.out.println("--- Integration ---");

        // 用 ArrayList + HashMap + for 循环做统计
        ArrayList numbers = new ArrayList();
        for (int i = 0; i < 10; i++) {
            numbers.add(i + 1);
        }

        int total = 0;
        for (int i = 0; i < numbers.size(); i++) {
            total = total + (int)(Integer(numbers.get(i)));
        }
        System.out.println("Total of 1..10 = " + total);

        HashMap stats = new HashMap();
        stats.put("count", numbers.size());
        stats.put("sum", total);
        stats.put("min", 1);
        stats.put("max", 10);

        StringBuilder report = new StringBuilder();
        report.append("Stats: [count=");
        report.append(stats.get("count") + "");
        report.append(", sum=");
        report.append(stats.get("sum") + "");
        report.append(", min=");
        report.append(stats.get("min") + "");
        report.append(", max=");
        report.append(stats.get("max") + "");
        report.append("]");
        System.out.println(report.toString());

        int sum_check = 0;
        if (stats.get("sum") != null) {
            sum_check = (int)(Integer(stats.get("sum")));
        }
        boolean all_ok = (sum_check == 55);
        System.out.println("Sum check (55 == 55)? " + all_ok);

        // =============================================================
        System.out.println("");
        System.out.println("=== ALL FEATURES DEMO COMPLETE ===");
    }
}
