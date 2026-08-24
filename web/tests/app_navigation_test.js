"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const test = require("node:test");
const vm = require("node:vm");

const navigationPath = path.join(__dirname, "..", "static", "app-navigation.js");
const navigationSource = fs.readFileSync(navigationPath, "utf8");

function namedFunctionSource(source, name) {
    const marker = "function " + name + "(";
    const start = source.indexOf(marker);
    assert.notEqual(start, -1, name + " is missing");
    const bodyStart = source.indexOf("{", start);
    let depth = 0;
    for (let index = bodyStart; index < source.length; index += 1) {
        if (source[index] === "{") depth += 1;
        if (source[index] === "}") depth -= 1;
        if (depth === 0) return source.slice(start, index + 1);
    }
    throw new Error(name + " body is incomplete");
}

test("soft navigation replaces and returns the server toast container", () => {
    const nextContainer = { id: "next" };
    const currentContainer = { id: "current" };
    const importedContainer = { id: "imported" };
    const replacements = [];
    const context = {
        document: {
            importNode(node, deep) {
                assert.equal(node, nextContainer);
                assert.equal(deep, true);
                return importedContainer;
            },
            querySelector(selector) {
                assert.equal(selector, "[data-toast-container]");
                return currentContainer;
            }
        },
        replaceElement(current, next) {
            replacements.push([current, next]);
        }
    };
    const syncToastContainer = vm.runInNewContext(
        "(" + namedFunctionSource(navigationSource, "syncToastContainer") + ")",
        context
    );
    const snapshot = {
        querySelector(selector) {
            assert.equal(selector, "[data-toast-container]");
            return nextContainer;
        }
    };

    assert.equal(syncToastContainer(snapshot), importedContainer);
    assert.deepEqual(replacements, [[currentContainer, importedContainer]]);
});

test("committed server toasts are hydrated after soft navigation", () => {
    assert.match(navigationSource, /var committedToastContainer = updateStableShell\(snapshot, targetURL\)/);
    assert.match(navigationSource, /EdgeApp\.hydratePage\(committedToastContainer\)/);
});
