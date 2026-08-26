package com.example.luajavademo;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Lua-Java Android Demo
 *
 * 原理: Java 源码 -> jlex/jparser 编译为 Lua 5.3 字节码 -> Lua VM 执行,
 * System.out.println 等输出通过管道捕获后显示在界面上。
 */
public class MainActivity extends Activity {

    static {
        System.loadLibrary("luajava_demo");
    }

    private native String nativeRunJava(String source);
    private native String nativeVersion();
    private native void nativeClose();

    /* ---------- 内置示例 ---------- */

    private static final String DEMO_HELLO =
            "public class HelloJavaOnLua {\n" +
            "    public static void main(String[] args) {\n" +
            "        System.out.println(\"Hello from Java on Lua VM (Android)!\");\n" +
            "        int a = 21;\n" +
            "        int b = 21;\n" +
            "        int sum = a + b;\n" +
            "        System.out.println(\"21 + 21 = \" + sum);\n" +
            "        if (sum == 42) {\n" +
            "            System.out.println(\"the answer to everything\");\n" +
            "        }\n" +
            "    }\n" +
            "}\n";

    private static final String DEMO_LOOP =
            "public class LoopDemo {\n" +
            "    public static void main(String[] args) {\n" +
            "        int total = 0;\n" +
            "        for (int i = 1; i <= 10; i = i + 1) {\n" +
            "            total = total + i;\n" +
            "        }\n" +
            "        System.out.println(\"sum(1..10) = \" + total);\n" +
            "\n" +
            "        int n = 5;\n" +
            "        while (n > 0) {\n" +
            "            System.out.println(\"countdown: \" + n);\n" +
            "            n = n - 1;\n" +
            "        }\n" +
            "    }\n" +
            "}\n";

    private static final String DEMO_COLLECTIONS =
            "import java.util.ArrayList;\n" +
            "import java.util.HashMap;\n" +
            "\n" +
            "public class CollectionsDemo {\n" +
            "    public static void main(String[] args) {\n" +
            "        ArrayList<String> fruits = new ArrayList<String>();\n" +
            "        fruits.add(\"Apple\");\n" +
            "        fruits.add(\"Banana\");\n" +
            "        fruits.add(\"Cherry\");\n" +
            "        for (String fruit : fruits) {\n" +
            "            System.out.println(\"fruit: \" + fruit);\n" +
            "        }\n" +
            "\n" +
            "        HashMap ages = new HashMap();\n" +
            "        ages.put(\"Alice\", 30);\n" +
            "        ages.put(\"Bob\", 25);\n" +
            "        System.out.println(\"Alice = \" + ages.get(\"Alice\"));\n" +
            "        System.out.println(\"map size = \" + ages.size());\n" +
            "    }\n" +
            "}\n";

    private static final String DEMO_CLASS =
            "public class Counter {\n" +
            "    private int value;\n" +
            "\n" +
            "    public Counter(int start) {\n" +
            "        this.value = start;\n" +
            "    }\n" +
            "\n" +
            "    public void add(int step) {\n" +
            "        this.value = this.value + step;\n" +
            "    }\n" +
            "\n" +
            "    public int get() {\n" +
            "        return this.value;\n" +
            "    }\n" +
            "\n" +
            "    public static void main(String[] args) {\n" +
            "        Counter c = new Counter(0);\n" +
            "        c.add(3);\n" +
            "        c.add(4);\n" +
            "        System.out.println(\"counter = \" + c.get());\n" +
            "    }\n" +
            "}\n";

    /* ---------- UI ---------- */

    private EditText mSourceEdit;
    private TextView mOutputView;
    private Button mRunButton;
    private final ExecutorService mExecutor = Executors.newSingleThreadExecutor();
    private final Handler mMainHandler = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        mSourceEdit = findViewById(R.id.source_edit);
        mOutputView = findViewById(R.id.output_view);
        mRunButton = findViewById(R.id.run_button);

        TextView titleView = findViewById(R.id.title_view);
        try {
            titleView.setText(getString(R.string.app_name) + "  |  " + nativeVersion());
        } catch (UnsatisfiedLinkError e) {
            titleView.setText("native 库加载失败: " + e.getMessage());
        }

        mSourceEdit.setText(DEMO_HELLO);
        mSourceEdit.setSelection(mSourceEdit.getText().length());

        mRunButton.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                runSource(mSourceEdit.getText().toString());
            }
        });

        bindExampleButton(R.id.btn_hello, DEMO_HELLO);
        bindExampleButton(R.id.btn_loop, DEMO_LOOP);
        bindExampleButton(R.id.btn_collections, DEMO_COLLECTIONS);
        bindExampleButton(R.id.btn_class, DEMO_CLASS);
    }

    private void bindExampleButton(int id, final String source) {
        findViewById(id).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                mSourceEdit.setText(source);
                mSourceEdit.setSelection(source.length());
            }
        });
    }

    private void runSource(final String source) {
        if (source.trim().isEmpty()) {
            Toast.makeText(this, "源码为空", Toast.LENGTH_SHORT).show();
            return;
        }
        mRunButton.setEnabled(false);
        mOutputView.setText("编译并执行中...");
        mExecutor.execute(new Runnable() {
            @Override
            public void run() {
                String result;
                try {
                    result = nativeRunJava(source);
                } catch (final Throwable t) {
                    result = "[NATIVE CRASH] " + t;
                }
                final String finalResult = result;
                mMainHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        mOutputView.setText(finalResult);
                        ScrollView sv = findViewById(R.id.output_scroll);
                        sv.fullScroll(View.FOCUS_UP);
                        mRunButton.setEnabled(true);
                    }
                });
            }
        });
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        mExecutor.shutdown();
        try {
            nativeClose();
        } catch (UnsatisfiedLinkError ignored) {
        }
    }
}
