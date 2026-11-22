//
//  NeoDroidBridge.h
//  NeoDroid iOS P1
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// 宿主/运行时架构自描述。
FOUNDATION_EXPORT NSString *NeoDroidArchitectureInfo(void);

/// P0 运行时自检，返回 "PASS|name|detail" / "FAIL|name|detail" 行。
FOUNDATION_EXPORT NSString *NeoDroidRunSelfTest(void);

/// P1：执行打包进 App 的 guest.elf，返回多行执行报告。
FOUNDATION_EXPORT NSString *NeoDroidRunGuestElf(void);

NS_ASSUME_NONNULL_END