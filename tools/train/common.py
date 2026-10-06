"""Shared helpers of the trainers: data sets per language, NOTE text, report.json merging."""
import json
import os
import random

import conllu

TREEBANKS = {
    "en": [("en_ewt", "UD_English-EWT", "CC BY-SA 4.0",
            "Universal Dependencies English Web Treebank, annotations (c) 2013-2021 The Board of Trustees of the "
            "Leland Stanford Junior University, CC BY-SA 4.0, https://github.com/UniversalDependencies/UD_English-EWT")],
    "es": [("es_gsd", "UD_Spanish-GSD", "CC BY-SA 4.0",
            "UD_Spanish-GSD (Google universal dependency treebank, converted to UD), CC BY-SA 4.0, "
            "https://github.com/UniversalDependencies/UD_Spanish-GSD"),
           ("es_ancora", "UD_Spanish-AnCora", "CC BY 4.0",
            "UD_Spanish-AnCora (AnCora corpus, University of Barcelona, converted to UD), CC BY 4.0, "
            "https://github.com/UniversalDependencies/UD_Spanish-AnCora")],
}
LANG_NAME = {"en": "english", "es": "spanish"}
REPORT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "report.json")


def load(lang, data_dir, split, max_sents=0, ancora_fraction=1.0, seed=7):
    """Returns (sentences, {treebank: (first, end)} index ranges, {file name: sha256})."""
    sents = []
    ranges = {}
    shas = {}
    for key, name, _lic, _att in TREEBANKS[lang]:
        path = os.path.join(data_dir, "%s-ud-%s.conllu" % (key, split))
        part = conllu.read(path)
        if split == "train" and key == "es_ancora" and ancora_fraction < 1.0:
            rnd = random.Random(seed)
            part = [s for s in part if rnd.random() < ancora_fraction]
        start = len(sents)
        sents.extend(part)
        ranges[name] = (start, len(sents))
        shas[os.path.basename(path)] = conllu.sha256_file(path)
    if max_sents:
        sents = sents[:max_sents]
    return sents, ranges, shas


def note_text(lang, kind, scores):
    lines = ["vetus poeta %s model (%s), trained by tools/train." % (kind, LANG_NAME[lang]),
             "Training data:"]
    for _key, _name, _lic, att in TREEBANKS[lang]:
        lines.append("  " + att)
    lines.append("The model weights are derived from these treebanks and are distributed under the same terms "
                 "(CC BY-SA 4.0 where any source is CC BY-SA).")
    lines.append("Scores: " + scores)
    return "\n".join(lines) + "\n"


def update_report(lang, section, value, path=REPORT):
    import tempfile
    try:
        import fcntl
    except ImportError:  # Windows: no concurrent trainers expected, no lock
        _update_report(lang, section, value, path)
        return
    with open(os.path.join(tempfile.gettempdir(), "vp_train_report.lock"), "w") as lk:
        fcntl.flock(lk, fcntl.LOCK_EX)
        _update_report(lang, section, value, path)


def _update_report(lang, section, value, path):
    try:
        with open(path, encoding="utf-8") as fh:
            rep = json.load(fh)
    except (OSError, ValueError):
        rep = {}
    rep.setdefault(lang, {})[section] = value
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(rep, fh, indent=2, sort_keys=True, ensure_ascii=False)
        fh.write("\n")
    os.replace(tmp, path)


TARGETS = {"en": {"upos": 94.0, "las": 78.0}, "es": {"upos": 95.0, "las": 82.0}}
SIZE_BUDGET = 12000000


def summarise(path=REPORT):
    """Adds a "summary" section per language: dev/test scores, targets, total bytes of the two files."""
    with open(path, encoding="utf-8") as fh:
        rep = json.load(fh)
    for lang, sec in rep.items():
        tag, dep = sec.get("tagger"), sec.get("parser")
        if not tag or not dep:
            continue
        total = tag["bytes"] + dep["bytes"]
        update_report(lang, "summary", {
            "dev_upos": tag["dev"]["upos"], "test_upos": tag["test"]["upos"],
            "dev_las": dep["dev"]["las"], "dev_uas": dep["dev"]["uas"],
            "test_las": dep["test"]["las"], "test_uas": dep["test"]["uas"],
            "target_upos": TARGETS[lang]["upos"], "target_las": TARGETS[lang]["las"],
            "targets_met": tag["dev"]["upos"] >= TARGETS[lang]["upos"] and dep["dev"]["las"] >= TARGETS[lang]["las"],
            "total_bytes": total, "size_budget_bytes": SIZE_BUDGET, "within_budget": total <= SIZE_BUDGET,
            "pruning_cost_dev_upos": round(tag["dev_float_unpruned"]["upos"] - tag["dev"]["upos"], 2)
            if "dev_float_unpruned" in tag else None,
            "pruning_cost_dev_las": round(dep["dev_float_unpruned"]["las"] - dep["dev"]["las"], 2)
            if "dev_float_unpruned" in dep else None,
            "note": "times measured with the four trainers running side by side on 4 cores",
        }, path)


if __name__ == "__main__":
    summarise()
