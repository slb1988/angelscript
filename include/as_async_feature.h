//@CYANCOOK(Async Compile Gate) BEGIN
#pragma once

// Shared by the vendor VM, engine integration and project consumers.
// Development defaults to ON. Override consistently for the entire build, never per translation unit.
#ifndef WITH_ANGELSCRIPT_ASYNC_AWAIT
#define WITH_ANGELSCRIPT_ASYNC_AWAIT 1
#endif
//@CYANCOOK(Async Compile Gate) END
