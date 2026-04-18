#include "persist_file.h"

namespace major_midi
{

FIL& SharedPersistFile()
{
    static FIL file;
    return file;
}

} // namespace major_midi
