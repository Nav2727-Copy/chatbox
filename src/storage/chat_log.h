/*
chatbox - a simple peer-to-peer chat application
written by "Nav2727" (what? you think i would put my real name on the internet?)
license: GNU GPLv3 (https://www.gnu.org/licenses/gpl-3.0.html)
*/
#pragma once

#include "common/common.h"
#include "common/utils.h"
class ChatLog
{
public:
    enum class Mode
    {
        File,
        StdoutOnly
    };

    explicit ChatLog(const std::string& path, Mode mode = Mode::File)
        : path_(path), mode_(mode)
    {
        if (mode_ == Mode::File)
        {
            file_.open(path_, std::ios::app);
            if (!file_.is_open())
                std::cerr << "[warn] Could not open log file: " << path_ << "\n";
        }

        write("=== Server started ===");
    }

    ~ChatLog()
    {
        if (mode_ == Mode::StdoutOnly || file_.is_open())
            write("=== Server stopped ===");

        if (file_.is_open())
            file_.close();
    }

    void write(const std::string& line)
    {
        std::lock_guard lock(mutex_);
        if (file_.is_open())
        {
            file_ << "[" << timestamp() << "] " << line << "\n";
            file_.flush();
        }
        std::cout << "[log] " << line << "\n";
    }

private:
    std::string path_;
    Mode mode_;
    std::ofstream file_;
    std::mutex mutex_;
};
