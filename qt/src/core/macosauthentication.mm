#include "macoskeychain.h"
#import <LocalAuthentication/LocalAuthentication.h>

namespace OKILTV::Core {
CFTypeRef macosAuthenticationContext()
{
    @autoreleasepool {
        LAContext *context = [[LAContext alloc] init];
        context.interactionNotAllowed = YES;
        return CFBridgingRetain(context);
    }
}
} // namespace OKILTV::Core
