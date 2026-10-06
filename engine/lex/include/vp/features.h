// Packed morphological features. [CONTRACT, DESIGN.md §5 FEAT] Identical enumerations in tools/build_library/features.py;
// tests/fixtures/features_golden.tsv is produced by the Python side and checked by the C++ side.
// Bit layout (LSB first): pos 0-4 | case 5-8 | number 9-10 | gender 11-13 | person 14-15 | tense 16-19 | mood 20-22
//                         | voice 23-24 | degree 25-26 | extra 27-31
#pragma once
#include <cstdint>

namespace vp::feat {
enum Pos : uint8_t { PosNone=0, Noun=1, Verb=2, Adj=3, Adv=4, Pron=5, Num=6, Prep=7, Conj=8, Intj=9, Det=10, Name=11,
                     Particle=12, Participle=13, Phrase=14, Suffix=15, Prefix=16, Article=17, Postp=18, Symbol=19,
                     Punct=20, PosOther=31 };
enum Case : uint8_t { CaseNone=0, Nom=1, Gen=2, Dat=3, Acc=4, Abl=5, Voc=6, Loc=7 };
enum Number : uint8_t { NumNone=0, Sg=1, Pl=2, Dual=3 };
enum Gender : uint8_t { GenNone=0, M=1, F=2, N=3, MF=4, MN=5, FN=6, MFN=7 };
enum Person : uint8_t { PersNone=0, P1=1, P2=2, P3=3 };
enum Tense : uint8_t { TenseNone=0, Present=1, Imperfect=2, Future=3, Perfect=4, Pluperfect=5, FuturePerfect=6, Aorist=7 };
enum Mood : uint8_t { MoodNone=0, Indicative=1, Subjunctive=2, Imperative=3, Infinitive=4, ParticipleMood=5, Gerund=6, Optative=7 };
enum Voice : uint8_t { VoiceNone=0, Active=1, Passive=2, Middle=3 };
enum Degree : uint8_t { DegNone=0, Positive=1, Comparative=2, Superlative=3 };
// extra: flag bits
enum Extra : uint8_t { Supine=1, Gerundive=2, Attic=4, Alternative=8, Contracted=16 };

struct Features {
  uint8_t pos=0, case_=0, number=0, gender=0, person=0, tense=0, mood=0, voice=0, degree=0, extra=0;
};

inline uint32_t pack(const Features& f) {
  return (uint32_t)(f.pos & 31) | ((uint32_t)(f.case_ & 15) << 5) | ((uint32_t)(f.number & 3) << 9) |
         ((uint32_t)(f.gender & 7) << 11) | ((uint32_t)(f.person & 3) << 14) | ((uint32_t)(f.tense & 15) << 16) |
         ((uint32_t)(f.mood & 7) << 20) | ((uint32_t)(f.voice & 3) << 23) | ((uint32_t)(f.degree & 3) << 25) |
         ((uint32_t)(f.extra & 31) << 27);
}
inline Features unpack(uint32_t p) {
  Features f;
  f.pos = p & 31; f.case_ = (p >> 5) & 15; f.number = (p >> 9) & 3; f.gender = (p >> 11) & 7; f.person = (p >> 14) & 3;
  f.tense = (p >> 16) & 15; f.mood = (p >> 20) & 7; f.voice = (p >> 23) & 3; f.degree = (p >> 25) & 3; f.extra = (p >> 27) & 31;
  return f;
}
}  // namespace vp::feat
