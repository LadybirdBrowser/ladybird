set(AK_TEST_SOURCES
    TestAllOf.cpp
    TestAnyOf.cpp
    TestArray.cpp
    TestAtomic.cpp
    TestBadge.cpp
    TestBase64.cpp
    TestBinaryHeap.cpp
    TestBinarySearch.cpp
    TestBitCast.cpp
    TestBitmap.cpp
    TestBitStream.cpp
    TestBuiltinWrappers.cpp
    TestByteBuffer.cpp
    TestByteString.cpp
    TestCharacterTypes.cpp
    TestChecked.cpp
    TestCircularBuffer.cpp
    TestCircularQueue.cpp
    TestDemangle.cpp
    TestDisjointChunks.cpp
    TestDistinctNumeric.cpp
    TestDoublyLinkedList.cpp
    TestEndian.cpp
    TestEnumBits.cpp
    TestEnumerate.cpp
    TestFind.cpp
    TestFixedArray.cpp
    TestFixedPoint.cpp
    TestFunction.cpp
    TestFlyString.cpp
    TestFormat.cpp
    TestGenericLexer.cpp
    TestGenericShorthands.cpp
    TestHashFunctions.cpp
    TestHashMap.cpp
    TestHashTable.cpp
    TestHex.cpp
    TestIPv4Address.cpp
    TestIPv6Address.cpp
    TestIndexSequence.cpp
    TestInsertionSort.cpp
    TestIntegerMath.cpp
    TestIntrusiveList.cpp
    TestIntrusiveRedBlackTree.cpp
    TestJSON.cpp
    TestLEB128.cpp
    TestMemory.cpp
    TestMemoryStream.cpp
    TestNeverDestroyed.cpp
    TestNonnullOwnPtr.cpp
    TestNonnullRefPtr.cpp
    TestNumberFormat.cpp
    TestOptional.cpp
    TestOptionParser.cpp
    TestOwnPtr.cpp
    TestQueue.cpp
    TestQuickSort.cpp
    TestRandom.cpp
    TestRedBlackTree.cpp
    TestRefPtr.cpp
    TestSegmentedVector.cpp
    TestSinglyLinkedList.cpp
    TestSourceGenerator.cpp
    TestSourceLocation.cpp
    TestSpan.cpp
    TestStack.cpp
    TestStdLibExtras.cpp
    TestString.cpp
    TestStringConversions.cpp
    TestStringUtils.cpp
    TestStringView.cpp
    TestTime.cpp
    TestTrie.cpp
    TestTuple.cpp
    TestTypeTraits.cpp
    TestTypedTransfer.cpp
    TestUFixedBigInt.cpp
    TestUtf16FlyString.cpp
    TestUtf16String.cpp
    TestUtf16View.cpp
    TestUtf8View.cpp
    TestVariant.cpp
    TestVector.cpp
    TestWeakPtr.cpp
)

# FIXME: LexicalPathWindows has some parenting and path parts sizing inconsistencies with LexicalPath, so it deserves
#  it's own platform-specific tests to avoid if-def soup in the Unix-based tests.
if(NOT WIN32)
    list(APPEND AK_TEST_SOURCES TestLexicalPath.cpp)
else()
    list(APPEND AK_TEST_SOURCES TestDelayLoadWindows.cpp)
endif()

foreach(source IN LISTS AK_TEST_SOURCES)
    ladybird_test("${source}" AK)
endforeach()

if (WIN32)
    # FIXME: Windows on ARM
    target_link_libraries(TestUFixedBigInt PRIVATE clang_rt.builtins-x86_64.lib)
endif()

if (CXX_COMPILER_SUPPORTS_BLOCKS)
    ladybird_test(TestFunctionObjC.mm AK NAME TestFunctionObjC)
    target_link_libraries(TestFunctionObjC PRIVATE ${BLOCKS_REQUIRED_LIBRARIES})
endif()
if (CXX_COMPILER_SUPPORTS_OBJC_ARC)
    ladybird_test(TestFunctionObjC.mm AK NAME TestFunctionObjCArc)
    target_compile_options(TestFunctionObjCArc PRIVATE -fobjc-arc)
    target_link_libraries(TestFunctionObjCArc PRIVATE ${BLOCKS_REQUIRED_LIBRARIES})
endif()

target_link_libraries(TestString PRIVATE LibUnicode)
target_link_libraries(TestUtf16String PRIVATE LibUnicode)
