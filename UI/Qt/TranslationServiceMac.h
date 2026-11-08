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

// The models the platform has for a language pair: The fast ones are the classic models, and the higher-quality ones
// the Apple Intelligence models. A supported model that isn't installed can be downloaded.
typedef NS_OPTIONS(NSInteger, LadybirdTranslationModels) {
    LadybirdTranslationModelsFastSupported = 1 << 0,
    LadybirdTranslationModelsFastInstalled = 1 << 1,
    LadybirdTranslationModelsHigherQualitySupported = 1 << 2,
    LadybirdTranslationModelsHigherQualityInstalled = 1 << 3,
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
+ (void)modelsFrom:(NSString*)source to:(NSArray<NSString*>*)targets completion:(void (^)(NSArray<NSNumber*>*))completion;
@end

API_AVAILABLE(macos(26.4))
@interface LadybirdTranslationSession : NSObject
- (instancetype)initWithSource:(NSString*)source target:(NSString*)target preferHigherQuality:(BOOL)preferHigherQuality;
- (instancetype)init NS_UNAVAILABLE;
- (void)translate:(NSArray<LadybirdTranslationRun*>*)runs completion:(void (^)(NSArray<LadybirdTranslationRun*>* _Nullable))completion;
- (void)cancel;
- (void)chooseModelAgainWithPreferHigherQuality:(BOOL)preferHigherQuality;
@end

NS_ASSUME_NONNULL_END
