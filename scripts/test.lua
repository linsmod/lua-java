-- scripts/test.lua
-- 供 C 主程序调用的 Lua 测试脚本

print("=== Lua Script: test.lua ===")
print("This is running inside the Lua VM embedded in C")

-- 调用 C 注册的函数
print("c_add(100, 200) = " .. c_add(100, 200))
print("c_uppercase('lua is great') = " .. c_uppercase("lua is great"))

-- 纯 Lua 逻辑
local function factorial(n)
    if n <= 1 then return 1 end
    return n * factorial(n - 1)
end

print("factorial(10) = " .. factorial(10))

-- table 遍历
local t = { name = "Lua", year = 1993, creator = "PUC-Rio" }
print("\nTable contents:")
for k, v in pairs(t) do
    print("  " .. k .. " -> " .. tostring(v))
end

-- 暴露给 C 端调用的函数
function greet(name)
    return "Hello, " .. name .. "! (from Lua)"
end

print("=== End of test.lua ===\n")
