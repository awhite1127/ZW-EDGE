import fs from "node:fs/promises";
import path from "node:path";
import { fileURLToPath } from "node:url";

const sourceURL = "https://raw.githubusercontent.com/drkameleon/complete-hsk-vocabulary/main/wordlists/inclusive/new/3.json";
const scriptDir = path.dirname(fileURLToPath(import.meta.url));
const outputPath = path.resolve(scriptDir, "../static/virtual-keyboard-pinyin-hsk-data.js");

function normalizeNumericPinyin(value) {
    return String(value || "")
        .toLowerCase()
        .replace(/u:|ü/g, "v")
        .replace(/[1-5]/g, "")
        .replace(/[^a-z']/g, "")
        .replace(/'/g, "");
}

async function downloadJSON(url) {
    let lastError;
    for (let attempt = 1; attempt <= 3; attempt += 1) {
        try {
            const response = await fetch(url);
            if (!response.ok) throw new Error(`HTTP ${response.status}`);
            return await response.json();
        } catch (error) {
            lastError = error;
            if (attempt < 3) await new Promise(resolve => setTimeout(resolve, attempt * 300));
        }
    }
    throw new Error(`下载 HSK 词库失败：${lastError?.message || "未知错误"}`);
}

const source = await downloadJSON(sourceURL);
source.sort((left, right) => {
    const leftFrequency = Number.isFinite(left.frequency) ? left.frequency : Number.MAX_SAFE_INTEGER;
    const rightFrequency = Number.isFinite(right.frequency) ? right.frequency : Number.MAX_SAFE_INTEGER;
    return leftFrequency - rightFrequency || String(left.simplified).localeCompare(String(right.simplified), "zh-CN");
});

const words = [];
const characters = [];
const seen = new Set();
for (const entry of source) {
    const text = String(entry.simplified || "").trim();
    if (!text) continue;
    for (const form of Array.isArray(entry.forms) ? entry.forms : []) {
        const pinyin = normalizeNumericPinyin(form?.transcriptions?.numeric);
        const identity = `${text}\u0000${pinyin}`;
        if (!pinyin || seen.has(identity)) continue;
        seen.add(identity);
        (Array.from(text).length === 1 ? characters : words).push([text, pinyin]);
    }
}

const output = `(function () {
    "use strict";

    /*
     * Complete HSK Vocabulary 新 HSK 1-3 精简输入索引。
     * Source: ${sourceURL}
     * License: MIT，完整文本见 virtual-keyboard-hsk-LICENSE.txt。
     * 本文件由 web/tools/generate-pinyin-hsk.mjs 生成，请勿手工维护条目。
     */
    window.EdgePinyinHSKLexicon = {
        words: ${JSON.stringify(words)},
        characters: ${JSON.stringify(characters)}
    };
}());
`;

await fs.writeFile(outputPath, output, "utf8");
console.log(`生成 ${outputPath}`);
console.log(`多字词读音 ${words.length} 条，单字读音 ${characters.length} 条`);
