/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// The Objective-C interface of the classes TranslationServiceMac.swift defines. The names here must match the @objc
// names there.

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, LadybirdTranslationLanguageStatus) {
    LadybirdTranslationLanguageStatusUnsupported,
    LadybirdTranslationLanguageStatusNeedsDownload,
    LadybirdTranslationLanguageStatusReady,
};

API_AVAILABLE(macos(26.4))
@interface LadybirdTranslationRun : NSObject
@property (nonatomic, readonly) NSString* text;
@property (nonatomic, readonly) uint32_t token;
@property (nonatomic, readonly) BOOL keepAsIs;
- (instancetype)initWithText:(NSString*)text token:(uint32_t)token keepAsIs:(BOOL)keepAsIs;
- (instancetype)init NS_UNAVAILABLE;
@end

API_AVAILABLE(macos(26.4))
@interface LadybirdTranslationService : NSObject
+ (void)statusesFrom:(NSString*)source to:(NSArray<NSString*>*)targets completion:(void (^)(NSArray<NSNumber*>*))completion;
@end

API_AVAILABLE(macos(26.4))
@interface LadybirdTranslationSession : NSObject
- (instancetype)initWithSource:(NSString*)source target:(NSString*)target;
- (instancetype)init NS_UNAVAILABLE;
- (void)translate:(NSArray<LadybirdTranslationRun*>*)runs completion:(void (^)(NSArray<LadybirdTranslationRun*>* _Nullable))completion;
- (void)cancel;
@end

NS_ASSUME_NONNULL_END
