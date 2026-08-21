(function () {
    "use strict";

    function normalize(value) {
        return String(value || "").toLowerCase().replace(/[^a-z']/g, "").replace(/'/g, "");
    }

    function addCandidate(target, seen, text, pinyin, tier, order, query, source) {
        var normalized = normalize(pinyin);
        if (!text || !normalized) return;
        var match = 0;
        if (normalized === query) match = 1500;
        else if (normalized.indexOf(query) === 0) match = 400;
        else if (query.indexOf(normalized) === 0) match = 300;
        else if (query.indexOf(normalized) >= 0) match = 200;
        else if (normalized.indexOf(query) >= 0) match = 100;
        if (!match) return;

        var score = tier + match - Math.min(order, 99);
        var existing = seen[text];
        if (existing && existing.score >= score) return;
        var candidate = { text: text, pinyin: normalized, score: score, source: source };
        seen[text] = candidate;
        if (existing) {
            var existingIndex = target.indexOf(existing);
            if (existingIndex >= 0) target.splice(existingIndex, 1);
        }
        target.push(candidate);
    }

    function industryEntries() {
        var words = window.EdgeKeyboardCommonWords || {};
        return {
            deviceTypes: Array.isArray(words.deviceTypes) ? words.deviceTypes : [],
            quantities: Array.isArray(words.quantities) ? words.quantities : []
        };
    }

    function baseEntries() {
        var projectLexicon = window.EdgePinyinBaseLexicon || {};
        var hskLexicon = window.EdgePinyinHSKLexicon || {};
        return {
            projectWords: Array.isArray(projectLexicon.words) ? projectLexicon.words : [],
            projectCharacters: Array.isArray(projectLexicon.characters) ? projectLexicon.characters : [],
            words: Array.isArray(hskLexicon.words) ? hskLexicon.words : [],
            characters: Array.isArray(hskLexicon.characters) ? hskLexicon.characters : []
        };
    }

    function characterReadingMap(base) {
        var result = Object.create(null);
        base.characters.concat(base.projectCharacters).forEach(function (entry) {
            var pinyin = normalize(entry[1]);
            if (!pinyin) return;
            if (!result[pinyin]) result[pinyin] = [];
            if (result[pinyin].indexOf(entry[0]) < 0 && result[pinyin].length < 8) {
                result[pinyin].push(entry[0]);
            }
        });
        return result;
    }

    function segmentPinyin(query, readingMap) {
        var paths = new Array(query.length + 1);
        paths[0] = [];
        for (var index = 0; index < query.length; index += 1) {
            if (!paths[index]) continue;
            for (var length = 6; length >= 1; length -= 1) {
                var end = index + length;
                if (end > query.length) continue;
                var syllable = query.slice(index, end);
                if (!readingMap[syllable]) continue;
                var candidate = paths[index].concat([syllable]);
                if (!paths[end] || candidate.length < paths[end].length) paths[end] = candidate;
            }
        }
        return paths[query.length] || [];
    }

    function composedCandidates(query, base) {
        var readingMap = characterReadingMap(base);
        var syllables = segmentPinyin(query, readingMap);
        if (syllables.length < 2 || syllables.length > 6) return [];
        var beam = [{ text: "", pinyin: "" }];
        syllables.forEach(function (syllable) {
            var next = [];
            beam.forEach(function (prefix) {
                readingMap[syllable].slice(0, 3).forEach(function (character) {
                    next.push({
                        text: prefix.text + character,
                        pinyin: prefix.pinyin + syllable
                    });
                });
            });
            beam = next.slice(0, 18);
        });
        return beam;
    }

    function search(rawQuery, recentWords) {
        var query = normalize(rawQuery);
        if (!query) return [];
        var results = [];
        var seen = Object.create(null);
        var industry = industryEntries();
        var base = baseEntries();

        // 千位分层固定业务优先级，百位匹配分再区分全拼、前缀和连续词组匹配。
        industry.deviceTypes.forEach(function (entry, index) {
            addCandidate(results, seen, entry.text, entry.pinyin, 5000, index, query, "device");
        });
        industry.quantities.forEach(function (entry, index) {
            addCandidate(results, seen, entry.text, entry.pinyin, 4500, index, query, "quantity");
        });

        (recentWords || []).forEach(function (word, index) {
            var known = industry.deviceTypes.concat(industry.quantities).filter(function (entry) {
                return entry.text === word;
            })[0];
            if (!known) {
                var baseKnown = base.projectWords.concat(base.projectCharacters, base.words, base.characters).filter(function (entry) {
                    return entry[0] === word;
                })[0];
                if (baseKnown) known = { text: baseKnown[0], pinyin: baseKnown[1] };
            }
            if (known) addCandidate(results, seen, known.text, known.pinyin, 4000, index, query, "recent");
        });

        base.projectWords.forEach(function (entry, index) {
            addCandidate(results, seen, entry[0], entry[1], 4000, index, query, "project-word");
        });
        base.projectCharacters.forEach(function (entry, index) {
            addCandidate(results, seen, entry[0], entry[1], 3400, index, query, "project-character");
        });
        base.words.forEach(function (entry, index) {
            addCandidate(results, seen, entry[0], entry[1], 3500, index, query, "word");
        });
        composedCandidates(query, base).forEach(function (entry, index) {
            addCandidate(results, seen, entry.text, entry.pinyin, 3200, index, query, "composition");
        });
        base.characters.forEach(function (entry, index) {
            addCandidate(results, seen, entry[0], entry[1], 3300, index, query, "character");
        });

        results.sort(function (left, right) {
            if (left.score !== right.score) return right.score - left.score;
            if (left.text.length !== right.text.length) return right.text.length - left.text.length;
            return left.text < right.text ? -1 : left.text > right.text ? 1 : 0;
        });
        return results;
    }

    window.EdgeOfflinePinyin = {
        normalize: normalize,
        search: search
    };
}());
