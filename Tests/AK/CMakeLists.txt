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
    TestSaturatingMath.cpp
    TestCircularBuffer.cpp
    TestCircularQueue.cpp
    TestDemangle.cpp
    TestDistinctNumeric.cpp
    TestDoublyLinkedList.cpp
    TestEndian.cpp
    TestEnumBits.cpp
    TestEnumerate.cpp
    TestFind.cpp
    TestFixedArray.cpp
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
    TestSinglyLinkedList.cpp
    TestSourceGenerator.cpp
    TestSourceLocation.cpp
    TestSpan.cpp
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

ladybird_test_suite(TestAK AK SOURCES ${AK_TEST_SOURCES} LIBS LibUnicode)

if (WIN32)
    # FIXME: Windows on ARM
    target_link_libraries(TestAK PRIVATE clang_rt.builtins-x86_64.lib)
endif()


if (APPLE)
    ladybird_test(TestFunctionObjC.mm AK NAME TestFunctionObjC)

    ladybird_test(TestFunctionObjC.mm AK NAME TestFunctionObjCArc)
    target_compile_options(TestFunctionObjCArc PRIVATE -fobjc-arc)
endif()
