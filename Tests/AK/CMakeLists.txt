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
    TestDisjointChunks.cpp
    TestDistinctNumeric.cpp
    TestDoublyLinkedList.cpp
    TestEndian.cpp
    TestEnumBits.cpp
    TestEnumerate.cpp
    TestFind.cpp
    TestFixedArray.cpp
    TestFixedPoint.cpp
    TestFloatingPointParsing.cpp
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
    TestLexicalPath.cpp
    TestMemory.cpp
    TestMemoryStream.cpp
    TestNeverDestroyed.cpp
    TestNonnullOwnPtr.cpp
    TestNonnullRefPtr.cpp
    TestNumberFormat.cpp
    TestOptional.cpp
    TestOptionParser.cpp
    TestOwnPtr.cpp
    TestPrint.cpp
    TestQueue.cpp
    TestQuickSort.cpp
    TestRedBlackTree.cpp
    TestRefPtr.cpp
    TestSegmentedVector.cpp
    TestSIMD.cpp
    TestSinglyLinkedList.cpp
    TestSourceGenerator.cpp
    TestSourceLocation.cpp
    TestSpan.cpp
    TestStack.cpp
    TestStdLibExtras.cpp
    TestString.cpp
    TestStringFloatingPointConversions.cpp
    TestStringUtils.cpp
    TestStringView.cpp
    TestDuration.cpp
    TestTrie.cpp
    TestTuple.cpp
    TestTypeTraits.cpp
    TestTypedTransfer.cpp
    TestUFixedBigInt.cpp
    TestUtf16.cpp
    TestUtf8.cpp
    TestVariant.cpp
    TestVector.cpp
    TestWeakPtr.cpp
)

foreach(source IN LISTS AK_TEST_SOURCES)
    serenity_test("${source}" AK)
endforeach()

if (CXX_COMPILER_SUPPORTS_BLOCKS)
    serenity_test(TestFunction.mm AK NAME TestFunction)
    target_link_libraries(TestFunction PRIVATE ${BLOCKS_REQUIRED_LIBRARIES})
endif()
if (CXX_COMPILER_SUPPORTS_OBJC_ARC)
    serenity_test(TestFunction.mm AK NAME TestFunctionArc)
    target_compile_options(TestFunctionArc PRIVATE -fobjc-arc)
    target_link_libraries(TestFunction PRIVATE ${BLOCKS_REQUIRED_LIBRARIES})
endif()

target_link_libraries(TestString PRIVATE LibUnicode)

if (ENABLE_SWIFT)

    # FIXME: Convert to use swift-testing after resolving https://github.com/LadybirdBrowser/ladybird/issues/1201
    add_executable(TestAKBindings TestAKBindings.swift)
    target_link_libraries(TestAKBindings PRIVATE AK)
    target_compile_options(TestAKBindings PRIVATE -parse-as-library)
    add_test(NAME TestAKBindings COMMAND TestAKBindings)
endif()
