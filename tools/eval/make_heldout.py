#!/usr/bin/env python3
"""Build the held-out subtitle file from a public-domain Project Gutenberg text (DESIGN §15, PREPLAN 6.5).
Usage: make_heldout.py <gutenberg_txt> <out.srt> [--max 800]
Takes quoted dialogue sentences between the Gutenberg START/END markers, splits them into subtitle-sized cues
(<= 84 characters, broken at clause punctuation), and gives each cue synthetic timing at 15 characters per second
with a 300 ms gap. Deterministic: same input gives the same output. The result is a test corpus, not a product file.
"""
import re, sys

def load_body(path):
    text = open(path, encoding='utf-8-sig').read()
    start = re.search(r'\*\*\* START OF THE PROJECT GUTENBERG EBOOK.*?\*\*\*', text)
    end = re.search(r'\*\*\* END OF THE PROJECT GUTENBERG EBOOK.*?\*\*\*', text)
    body = text[start.end():end.start()] if start and end else text
    # skip front matter: begin at the first chapter heading when there is one
    m = re.search(r'\bChapter\s+I\b', body)
    if m:
        body = body[m.start():]
    return re.sub(r'\s+', ' ', body)

def dialogue_sentences(body):
    out = []
    for q in re.findall(r'“([^”]{8,400})”|"([^"]{8,400})"', body):
        q = (q[0] or q[1]).strip()
        for s in re.split(r'(?<=[.!?])\s+', q):
            s = s.strip()
            if 10 <= len(s) <= 200 and re.search(r'[A-Za-z]', s) and s[-1] in '.!?':
                out.append(s)
    return out

def cues_from_sentence(s, maxlen=84):
    if len(s) <= maxlen:
        return [s]
    parts, cur = [], ''
    for chunk in re.split(r'(?<=[,;:])\s+', s):
        if cur and len(cur) + 1 + len(chunk) > maxlen:
            parts.append(cur); cur = chunk
        else:
            cur = (cur + ' ' + chunk).strip()
    if cur: parts.append(cur)
    return parts

def fmt(ms):
    h, ms = divmod(ms, 3600000); m, ms = divmod(ms, 60000); s, ms = divmod(ms, 1000)
    return '%02d:%02d:%02d,%03d' % (h, m, s, ms)

def main():
    src, dst = sys.argv[1], sys.argv[2]
    limit = int(sys.argv[sys.argv.index('--max') + 1]) if '--max' in sys.argv else 800
    cues = []
    for s in dialogue_sentences(load_body(src)):
        cues.extend(cues_from_sentence(s))
        if len(cues) >= limit: break
    cues = cues[:limit]
    t = 1000; lines = []
    for i, c in enumerate(cues, 1):
        dur = max(1200, int(len(c) / 15.0 * 1000))
        lines.append('%d\n%s --> %s\n%s\n' % (i, fmt(t), fmt(t + dur), c))
        t += dur + 300
    with open(dst, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines))
    print('cues', len(cues), 'chars', sum(len(c) for c in cues))

if __name__ == '__main__':
    main()
