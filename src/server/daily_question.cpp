#include "server/daily_question.h"

#include <cstdio>

namespace judas {

const std::vector<std::string> & daily_questions() {
    static const std::vector<std::string> q = {
        "If I were a color, which one would I be and why?",
        "What would my superpower be, based on what you know about me?",
        "Which tiny thing made you smile today?",
        "If we could teleport anywhere for one lunch, where would we eat?",
        "What is a song that fits my mood this week?",
        "Which animal do I remind you of, and what is the reason?",
        "If I opened a shop, what would it sell?",
        "What is something I do that you secretly love?",
        "Which fictional world would I survive in the longest?",
        "What is the best small adventure we could have this weekend?",
        "If I wrote a book, what would the title be?",
        "What is one thing I should be prouder of?",
        "Which decade would I have fit into best?",
        "What is a smell that takes you straight to a memory?",
        "If I had a theme song walking into a room, what would it be?",
        "What is a skill you think I would be great at if I tried it?",
        "Which snack is basically my personality?",
        "What would my talk show be about?",
        "If we swapped jobs for a day, what would go wrong first?",
        "What is the most underrated invention, in your opinion?",
        "Which three words describe how I have been lately?",
        "If I could master any instrument overnight, which one should I pick?",
        "What is a place we have not been that I would love?",
        "What would my villain origin story be, and how silly is it?",
        "Which of my ideas deserves another look?",
        "What is the nicest thing a stranger has done for you?",
        "If my life were a video game, what is my special move?",
        "What is a harmless habit of mine that makes you laugh?",
        "Which season am I, and why?",
        "What is one tradition we should start?",
        "If I were a vehicle, which would I be?",
        "What is a food I should try at least once more?",
        "What was the best part of your week so far?",
        "Which movie scene would I recreate perfectly?",
        "What would I name a pet dragon?",
        "If I could redesign one everyday object, which should it be?",
        "What is something small that would make tomorrow better?",
        "Which of my old hobbies should come back?",
        "What would the weather be if it matched my mood today?",
        "If I started a band, what would we be called?",
        "What is a compliment you wish you heard more often?",
        "Which room in a house do I belong in most?",
        "If I ran a restaurant, what is the signature dish?",
        "What is a question you have always wanted to ask me?",
        "Which tiny luxury would make the biggest difference this week?",
        "What would my autobiography chapter for this year be called?",
        "If we built a secret hideout, what would be inside?",
        "What is one thing I taught you without meaning to?",
        "Which animal sidekick would I pick, and what would it be like?",
        "What is the best sound in the world?",
        "If I could have dinner with anyone from history, who would I invite?",
        "What is a rule I would make if I were in charge for a day?",
        "Which of my quirks would be a legendary trait in a fantasy game?",
        "What did you dream about most recently, as far as you remember?",
        "If I were a planet, what would the weather be like?",
        "What do I do that makes an ordinary day better?",
        "Which gadget do I need that does not exist yet?",
        "What is one small win I should celebrate today?",
        "If I could live in any decade of music, which one would it be?",
        "What is a story from my childhood you wish you had seen?",
        "What is the weirdest talent you think I might secretly have?",
    };
    return q;
}

long daily_day_number(int y, int m, int d) {
    // days_from_civil (Howard Hinnant)
    y -= m <= 2;
    const long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned) (y - era * 400);
    const unsigned doy = (unsigned) ((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long) doe - 719468;
}

std::string daily_question_for(int y, int m, int d) {
    const auto & q = daily_questions();
    long n = daily_day_number(y, m, d) % (long) q.size();
    if (n < 0) n += (long) q.size();
    return q[(size_t) n];
}

bool daily_parse_date(const std::string & s, int & y, int & m, int & d) {
    char tail = 0;
    if (std::sscanf(s.c_str(), "%4d-%2d-%2d%c", &y, &m, &d, &tail) != 3) return false;
    if (s.size() != 10 || y < 1970 || y > 2200 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    static const int dim[] = {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (d > dim[m - 1]) return false;
    if (m == 2 && d == 29 && !((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return false;
    return true;
}

}  // namespace judas
