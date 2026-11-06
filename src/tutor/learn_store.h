// Learn v2 storage: per-topic phase, plan graph and noted propositions, in <data_dir>/learn/topics.json.
// Pure functions work on a json document so tests need no files; the load/save helpers add the file and a lock.
#pragma once

#include <mutex>
#include <string>

#include "core/config.h"
#include "core/json.h"

namespace judas {

// "Hash tables 101" -> "hash-tables-101" (lowercase a-z 0-9 and single dashes, at most 48 chars, "" if nothing left).
std::string learn_slug(const std::string & name);

// Matching form of a sentence: lowercase ASCII letters and digits, any other run of characters (punctuation,
// whitespace) collapsed to one space, ends trimmed. "Chaining  stores, collisions!" == "chaining stores collisions".
std::string learn_norm(const std::string & text);
// The id of the proposition a premise points at: its short id ("p3", "P3", "#p3") or its text under learn_norm.
// "" when it names none. props is the topic's "props" array.
std::string learn_premise_ref(const json & props, const std::string & premise);

std::string learn_topics_path(const Config & cfg);
std::mutex & learn_topics_mutex();
// {"topics": {id: {...}}}; a missing or damaged file gives an empty document.
json learn_topics_load(const Config & cfg);
bool learn_topics_save(const Config & cfg, const json & doc, std::string * err);

// learn_plan: {topic, phase?, steps?[{id,title,depends_on?,status?}], update?[{id,status}]}. steps replaces the whole plan.
// Returns the reply for the model; *ok false (and doc untouched) on bad input.
// Moves the old learn_progress record (<data_dir>/learn/progress.json) into the topics document, once: a topic that
// already exists is left alone, "migrated" marks the document so it is not repeated. Returns true when doc changed.
bool learn_progress_migrate(json & doc, const json & progress);
// Loads topics.json and, the first time, folds progress.json into it and saves. progress.json is never changed.
json learn_topics_load_migrated(const Config & cfg);

std::string learn_plan_apply(json & doc, const json & args, bool * ok, const std::string & today, const std::string & session);
// learn_note: {topic, notes: [{text, premises?[]}] (1 to 3, or plain strings)}. A premise that names an earlier
// proposition (short id such as "p2", or its words) is stored as that id; an id that does not exist is refused.
std::string learn_note_apply(json & doc, const json & args, bool * ok, const std::string & today, const std::string & session);

// One line per topic for GET /api/learn/topics, newest first.
json learn_topics_list(const json & doc);
// Full topic with its id, or null when unknown.
json learn_topic_get(const json & doc, const std::string & id);
// What a new Learn session should see: recent propositions per topic, newest topics first, at most max_bytes.
std::string learn_knowledge_digest(const json & doc, size_t max_bytes);

}  // namespace judas
