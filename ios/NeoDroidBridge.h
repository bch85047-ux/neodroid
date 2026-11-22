//
//  NeoDroidBridge.h
//  NeoDroid iOS P0
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// 返回宿主/运行时架构与能力自描述。
FOUNDATION_EXPORT NSString *NeoDroidArchitectureInfo(void);

/// 运行 P0 运行时自检，返回 "PASS|name|detail" / "FAIL|name|detail" 行。
FOUNDATION_EXPORT NSString *NeoDroidRunSelfTest(void);

NS_ASSUME_NONNULL_END
