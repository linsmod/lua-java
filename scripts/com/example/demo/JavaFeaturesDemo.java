package com.example.demo;

import java.util.ArrayList;
import java.util.List;

/**
 * 这个类展示了 Java 语言的主要语法和特性。
 */
public class JavaFeaturesDemo {

    // 实例变量
    private String name;
    private int age;

    // 常量
    public static final double PI = 3.14159;

    // 枚举类型
    public enum Day {
        MONDAY, TUESDAY, WEDNESDAY, THURSDAY, FRIDAY, SATURDAY, SUNDAY
    }

    // 构造器
    public JavaFeaturesDemo(String name, int age) {
        this.name = name;
        this.age = age;
    }

    // 实例方法
    public void displayInfo() {
        System.out.println("Name: " + name + ", Age: " + age);
    }

    // 方法重载 - disabled for testing
    // public void displayInfo(String prefix) {
    //     System.out.println(prefix + " Name: " + name + ", Age: " + age);
    // }

    // 静态方法
    public static int add(int a, int b) {
        return a + b;
    }

    // 可变参数
    public static int sum(int... numbers) {
        int total = 0;
        for (int num : numbers) {
            total += num;
        }
        return total;
    }

    // 抛出异常的方法
    public static int divide(int a, int b) throws ArithmeticException {
        if (b == 0) {
            throw new ArithmeticException("Cannot divide by zero");
        }
        return a / b;
    }

    public static void main(String[] args) {
        // ---------- 基本数据类型 ----------
        int intVar = 10;
        double doubleVar = 20.5;
        boolean boolVar = true;
        char charVar = 'A';
        String stringVar = "Hello Java";  // String 是引用类型，但使用频繁

        System.out.println("intVar = " + intVar);
        System.out.println("doubleVar = " + doubleVar);
        System.out.println("boolVar = " + boolVar);
        System.out.println("charVar = " + charVar);
        System.out.println("stringVar = " + stringVar);

        // ---------- 条件语句 ----------
        if (intVar > 5) {
            System.out.println("intVar is greater than 5");
        } else {
            System.out.println("intVar is <= 5");
        }

        // switch 语句（支持枚举）
        Day today = Day.MONDAY;
        switch (today) {
            case MONDAY:
                System.out.println("Start of work week");
                break;
            case FRIDAY:
                System.out.println("TGIF");
                break;
            default:
                System.out.println("Midweek");
                break;
        }

        // ---------- 循环 ----------
        // for 循环
        for (int i = 0; i < 5; i++) {
            System.out.print(i + " ");
        }
        System.out.println();

        // while 循环
        int j = 0;
        while (j < 3) {
            System.out.print(j + " ");
            j++;
        }
        System.out.println();

        // do-while 循环
        int k = 0;
        do {
            System.out.print(k + " ");
            k++;
        } while (k < 3);
        System.out.println();

        // ---------- 数组和增强 for ----------
        int[] numbers = {1, 2, 3, 4, 5};
        for (int num : numbers) {
            System.out.print(num + " ");
        }
        System.out.println();

        // ---------- 静态方法调用 ----------
        int sumResult = add(5, 7);
        System.out.println("Sum of 5 and 7 = " + sumResult);

        // ---------- 可变参数 ----------
        int total = sum(1, 2, 3, 4, 5);
        System.out.println("Sum of 1..5 = " + total);

        // ---------- 对象创建和实例方法 ----------
        JavaFeaturesDemo demo = new JavaFeaturesDemo("Alice", 30);
        System.out.println("demo created successfully");
        // demo.displayInfo();  // test
        // demo.displayInfo("Person: ");

        // ---------- 异常处理 ----------
        // try { ... }

        // ---------- 集合（ArrayList） ----------
        // ...

        // ---------- 常量 ----------
        System.out.println("PI = " + PI);
    }
}