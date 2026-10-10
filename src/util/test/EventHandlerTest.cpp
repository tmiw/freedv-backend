// Tests for EventHandler: += adds a handler, = replaces them all, clear()
// removes them, and every handler gets the same arguments.

#include <iostream>
#include <string>
#include <vector>

#include "../EventHandler.h"

namespace {

bool report(bool result)
{
    std::cout << (result ? "PASS" : "FAIL") << "\n";
    return result;
}

bool testAddCallsAllInOrder()
{
    std::cout << "Test 1 (+= adds handlers, called in order): ";

    EventHandler<int> handler;
    std::vector<std::string> calls;
    handler += [&](int v) { calls.push_back("a" + std::to_string(v)); };
    handler += [&](int v) { calls.push_back("b" + std::to_string(v)); };
    handler(7);

    return report(calls == std::vector<std::string>{"a7", "b7"});
}

bool testAssignReplaces()
{
    std::cout << "Test 2 (= replaces every handler; clear() removes them): ";

    EventHandler<int> handler;
    int first = 0, second = 0, third = 0;
    handler += [&](int) { first++; };
    handler += [&](int) { second++; };
    handler = [&](int) { third++; };
    handler(1);
    bool result = first == 0 && second == 0 && third == 1;

    handler.clear();
    handler(1);
    result &= third == 1;

    EventHandler<> empty;
    empty(); // no handlers: nothing to call
    return report(result);
}

bool testEveryHandlerGetsTheArguments()
{
    std::cout << "Test 3 (every handler gets by-value arguments intact): ";

    // Used to forward the arguments to each handler in turn, so the first
    // handler moved the string out and the rest got an empty one.
    EventHandler<std::string, std::vector<int>> handler;
    std::vector<std::string> strings;
    std::vector<size_t> sizes;
    for (int i = 0; i < 3; i++)
    {
        handler += [&](std::string s, std::vector<int> v) {
            strings.push_back(s);
            sizes.push_back(v.size());
        };
    }
    handler(std::string("a string too long for the small-string buffer"), std::vector<int>{1, 2, 3});

    bool result = strings.size() == 3 && sizes.size() == 3;
    for (size_t i = 0; result && i < 3; i++)
    {
        result &= strings[i] == "a string too long for the small-string buffer" && sizes[i] == 3;
    }

    // References still refer to the caller's object.
    EventHandler<std::string&> refHandler;
    refHandler += [](std::string& s) { s += "1"; };
    refHandler += [](std::string& s) { s += "2"; };
    std::string target = "x";
    refHandler(target);
    result &= target == "x12";

    return report(result);
}

} // namespace

int main()
{
    bool result = true;
    result &= testAddCallsAllInOrder();
    result &= testAssignReplaces();
    result &= testEveryHandlerGetsTheArguments();
    return result ? 0 : -1;
}
