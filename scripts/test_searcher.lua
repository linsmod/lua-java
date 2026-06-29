print("searchers count:", #package.searchers)
for i = 1, #package.searchers do
    print(string.format("  [%d] %s", i, type(package.searchers[i])))
end
-- Try index 5
print("  [5]", type(package.searchers[5]))
