// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Alviro Iskandar Setiawan <alviro.iskandar@gnuweeb.org>
 */
#ifndef GWDISCORD__LOG_HPP
#define GWDISCORD__LOG_HPP

#include <functional>
#include <string>

namespace gwdiscord {

enum class LogLevel {
	Error,
	Warn,
	Info,
	Debug,
};

/*
 * Where the library reports what it is doing. gwdiscord deliberately owns no
 * logging machinery: the host injects a sink and adapts it to whatever it
 * already uses. A default-constructed (empty) sink discards everything, so a
 * caller that does not care can pass {}.
 */
using LogSink = std::function<void(LogLevel, const std::string &)>;

const char *log_level_name(LogLevel lvl);

} /* namespace gwdiscord */

#endif /* #ifndef GWDISCORD__LOG_HPP */
