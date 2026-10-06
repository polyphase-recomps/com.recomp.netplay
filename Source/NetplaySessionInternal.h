/**
 * @file NetplaySessionInternal.h
 * @brief What the addon itself sets up in the session (not for game players).
 */
#pragma once

namespace NetplaySessionInternal
{
typedef void (*LogFn)(const char* line);
void SetLogger(LogFn log);
}
