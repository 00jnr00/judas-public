// The phone app's daily question: one light, personal question per local day, picked from a built-in list.
#pragma once
#include <string>
#include <vector>

namespace judas {

const std::vector<std::string> & daily_questions();
// Days since 1970-01-01 for a calendar date (proleptic Gregorian).
long daily_day_number(int year, int month, int day);
// The question for a calendar date: same date, same question; consecutive days differ.
std::string daily_question_for(int year, int month, int day);
// Parses "YYYY-MM-DD"; false when it is not a valid date.
bool daily_parse_date(const std::string & s, int & y, int & m, int & d);

}  // namespace judas
