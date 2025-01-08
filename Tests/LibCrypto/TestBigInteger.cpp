/*
 * Copyright (c) 2021, Peter Bocan  <me@pbocan.net>
 * Copyright (c) 2025, Manuel Zahariev  <manuel@duck.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Tuple.h>
#include <LibCrypto/BigInt/Algorithms/UnsignedBigIntegerAlgorithms.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibCrypto/BigInt/UnsignedBigInteger.h>
#include <LibCrypto/NumberTheory/ModularFunctions.h>
#include <LibTest/TestCase.h>
#include <math.h>

static Crypto::UnsignedBigInteger bigint_fibonacci(size_t n)
{
    Crypto::UnsignedBigInteger num1(0);
    Crypto::UnsignedBigInteger num2(1);
    for (size_t i = 0; i < n; ++i) {
        Crypto::UnsignedBigInteger t = num1.plus(num2);
        num2 = num1;
        num1 = t;
    }
    return num1;
}

static Crypto::SignedBigInteger bigint_signed_fibonacci(size_t n)
{
    Crypto::SignedBigInteger num1(0);
    Crypto::SignedBigInteger num2(1);
    for (size_t i = 0; i < n; ++i) {
        Crypto::SignedBigInteger t = num1.plus(num2);
        num2 = num1;
        num1 = t;
    }
    return num1;
}

TEST_CASE(test_bigint_fib500)
{
    Vector<u32> result {
        315178285, 505575602, 1883328078, 125027121, 3649625763,
        347570207, 74535262, 3832543808, 2472133297, 1600064941, 65273441
    };

    EXPECT_EQ(bigint_fibonacci(500).words(), result);
}

TEST_CASE(test_unsigned_bigint_addition_initialization)
{
    Crypto::UnsignedBigInteger num1;
    Crypto::UnsignedBigInteger num2(70);
    Crypto::UnsignedBigInteger num3 = num1.plus(num2);
    bool pass = (num3 == num2);
    pass &= (num1 == Crypto::UnsignedBigInteger(0));
    EXPECT(pass);
}

TEST_CASE(test_unsigned_bigint_addition_borrow_with_zero)
{
    Crypto::UnsignedBigInteger num1({ UINT32_MAX - 3, UINT32_MAX });
    Crypto::UnsignedBigInteger num2({ UINT32_MAX - 2, 0 });
    Vector<u32> expected_result { 4294967289, 0, 1 };
    EXPECT_EQ(num1.plus(num2).words(), expected_result);
}

TEST_CASE(test_unsigned_bigint_basic_add_to_accumulator)
{
    Crypto::UnsignedBigInteger num1(10);
    Crypto::UnsignedBigInteger num2(70);
    Crypto::UnsignedBigIntegerAlgorithms::add_into_accumulator_without_allocation(num1, num2);
    EXPECT_EQ(num1.words(), Vector<u32> { 80 });
}

TEST_CASE(test_unsigned_bigint_basic_add_to_empty_accumulator)
{
    Crypto::UnsignedBigInteger num1 {};
    Crypto::UnsignedBigInteger num2(10);
    Crypto::UnsignedBigIntegerAlgorithms::add_into_accumulator_without_allocation(num1, num2);
    EXPECT_EQ(num1.words(), Vector<u32> { 10 });
}

TEST_CASE(test_unsigned_bigint_basic_add_to_smaller_accumulator)
{
    Crypto::UnsignedBigInteger num1(10);
    Crypto::UnsignedBigInteger num2({ 10, 10 });
    Crypto::UnsignedBigIntegerAlgorithms::add_into_accumulator_without_allocation(num1, num2);
    Vector<u32> expected_result { 20, 10 };
    EXPECT_EQ(num1.words(), expected_result);
}

TEST_CASE(test_unsigned_bigint_add_to_accumulator_with_multiple_carry_levels)
{
    Crypto::UnsignedBigInteger num1({ UINT32_MAX - 2, UINT32_MAX });
    Crypto::UnsignedBigInteger num2(5);
    Crypto::UnsignedBigIntegerAlgorithms::add_into_accumulator_without_allocation(num1, num2);
    Vector<u32> expected_result { 2, 0, 1 };
    EXPECT_EQ(num1.words(), expected_result);
}

TEST_CASE(test_unsigned_bigint_add_to_accumulator_with_leading_zero)
{
    Crypto::UnsignedBigInteger num1(1);
    Crypto::UnsignedBigInteger num2({ 1, 0 });
    Crypto::UnsignedBigIntegerAlgorithms::add_into_accumulator_without_allocation(num1, num2);
    EXPECT_EQ(num1.words(), Vector<u32> { 2 });
}

TEST_CASE(test_unsigned_bigint_add_to_accumulator_with_carry_and_leading_zero)
{
    Crypto::UnsignedBigInteger num1({ UINT32_MAX, 0, 0, 0 });
    Crypto::UnsignedBigInteger num2({ 1, 0 });
    Crypto::UnsignedBigIntegerAlgorithms::add_into_accumulator_without_allocation(num1, num2);
    Vector<u32> expected_result { 0, 1, 0, 0 };
    EXPECT_EQ(num1.words(), expected_result);
}

TEST_CASE(test_unsigned_bigint_simple_subtraction)
{
    Crypto::UnsignedBigInteger num1(80);
    Crypto::UnsignedBigInteger num2(70);

    EXPECT_EQ(num1.minus(num2), Crypto::UnsignedBigInteger(10));
}

TEST_CASE(test_unsigned_bigint_simple_subtraction_invalid)
{
    Crypto::UnsignedBigInteger num1(50);
    Crypto::UnsignedBigInteger num2(70);

    EXPECT(num1.minus(num2).is_invalid());
}

TEST_CASE(test_unsigned_bigint_simple_subtraction_with_borrow)
{
    Crypto::UnsignedBigInteger num1(UINT32_MAX);
    Crypto::UnsignedBigInteger num2(1);
    Crypto::UnsignedBigInteger num3 = num1.plus(num2);
    Crypto::UnsignedBigInteger result = num3.minus(num2);
    EXPECT_EQ(result, num1);
}

TEST_CASE(test_unsigned_bigint_subtraction_with_large_numbers)
{
    Crypto::UnsignedBigInteger num1 = bigint_fibonacci(343);
    Crypto::UnsignedBigInteger num2 = bigint_fibonacci(218);
    Crypto::UnsignedBigInteger result = num1.minus(num2);

    Vector<u32> expected_result {
        811430588, 2958904896, 1130908877, 2830569969, 3243275482,
        3047460725, 774025231, 7990
    };
    EXPECT_EQ(result.plus(num2), num1);
    EXPECT_EQ(result.words(), expected_result);
}

TEST_CASE(test_unsigned_bigint_subtraction_with_large_numbers2)
{
    Crypto::UnsignedBigInteger num1(Vector<u32> { 1483061863, 446680044, 1123294122, 191895498, 3347106536, 16, 0, 0, 0 });
    Crypto::UnsignedBigInteger num2(Vector<u32> { 4196414175, 1117247942, 1123294122, 191895498, 3347106536, 16 });
    Crypto::UnsignedBigInteger result = num1.minus(num2);
    // this test only verifies that we don't crash on an assertion
}

TEST_CASE(test_unsigned_bigint_subtraction_regression_1)
{
    auto num = Crypto::UnsignedBigInteger { 1 }.shift_left(256);
    Vector<u32> expected_result {
        4294967295, 4294967295, 4294967295, 4294967295, 4294967295,
        4294967295, 4294967295, 4294967295, 0
    };
    EXPECT_EQ(num.minus(1).words(), expected_result);
}

TEST_CASE(test_unsigned_bigint_simple_multiplication)
{
    Crypto::UnsignedBigInteger num1(8);
    Crypto::UnsignedBigInteger num2(251);
    Crypto::UnsignedBigInteger result = num1.multiplied_by(num2);
    EXPECT_EQ(result.words(), Vector<u32> { 2008 });
}

TEST_CASE(test_unsigned_bigint_multiplication_with_big_numbers1)
{
    Crypto::UnsignedBigInteger num1 = bigint_fibonacci(200);
    Crypto::UnsignedBigInteger num2(12345678);
    Crypto::UnsignedBigInteger result = num1.multiplied_by(num2);
    Vector<u32> expected_result { 669961318, 143970113, 4028714974, 3164551305, 1589380278, 2 };
    EXPECT_EQ(result.words(), expected_result);
}

TEST_CASE(test_unsigned_bigint_multiplication_with_big_numbers2)
{
    Crypto::UnsignedBigInteger num1 = bigint_fibonacci(200);
    Crypto::UnsignedBigInteger num2 = bigint_fibonacci(341);
    Crypto::UnsignedBigInteger result = num1.multiplied_by(num2);
    Vector<u32> expected_result {
        3017415433, 2741793511, 1957755698, 3731653885, 3154681877,
        785762127, 3200178098, 4260616581, 529754471, 3632684436,
        1073347813, 2516430
    };
    EXPECT_EQ(result.words(), expected_result);
}

TEST_CASE(test_unsigned_bigint_simple_division)
{
    Crypto::UnsignedBigInteger num1(27194);
    Crypto::UnsignedBigInteger num2(251);
    auto result = num1.divided_by(num2);
    Crypto::UnsignedDivisionResult expected = { Crypto::UnsignedBigInteger(108), Crypto::UnsignedBigInteger(86) };
    EXPECT_EQ(result.quotient, expected.quotient);
    EXPECT_EQ(result.remainder, expected.remainder);
}

TEST_CASE(test_unsigned_bigint_division_with_big_numbers)
{
    Crypto::UnsignedBigInteger num1 = bigint_fibonacci(386);
    Crypto::UnsignedBigInteger num2 = bigint_fibonacci(238);
    auto result = num1.divided_by(num2);
    Crypto::UnsignedDivisionResult expected = {
        Crypto::UnsignedBigInteger(Vector<u32> { 2300984486, 2637503534, 2022805584, 107 }),
        Crypto::UnsignedBigInteger(Vector<u32> { 1483061863, 446680044, 1123294122, 191895498, 3347106536, 16, 0, 0, 0 })
    };
    EXPECT_EQ(result.quotient, expected.quotient);
    EXPECT_EQ(result.remainder, expected.remainder);
}

TEST_CASE(test_unsigned_bigint_division_combined_test)
{
    auto num1 = bigint_fibonacci(497);
    auto num2 = bigint_fibonacci(238);
    auto div_result = num1.divided_by(num2);
    EXPECT_EQ(div_result.quotient.multiplied_by(num2).plus(div_result.remainder), num1);
}

TEST_CASE(test_unsigned_bigint_base10_from_string)
{
    auto result = TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(10, "57195071295721390579057195715793"sv));
    Vector<u32> expected_result { 3806301393, 954919431, 3879607298, 721 };
    EXPECT_EQ(result.words(), expected_result);

    Vector<StringView> invalid_base10_number_strings { "1A"sv, "1:"sv, "Z1"sv, "1/"sv };
    for (auto invalid_base10_number_string : invalid_base10_number_strings)
        EXPECT_EQ(Crypto::UnsignedBigInteger::from_base(10, invalid_base10_number_string).is_error(), true);
}

TEST_CASE(test_unsigned_bigint_base10_to_string)
{
    auto bigint = Crypto::UnsignedBigInteger {
        Vector<u32> { 3806301393, 954919431, 3879607298, 721 }
    };
    auto result = MUST(bigint.to_base(10));
    EXPECT_EQ(result, "57195071295721390579057195715793");
}

TEST_CASE(test_bigint_modular_inverse)
{
    auto result = Crypto::NumberTheory::ModularInverse(7, 87);
    EXPECT_EQ(result, 25);

    // RSA-like calculations (non-prime modulus)
    // 256 bits
    auto result0 = Crypto::NumberTheory::ModularInverse("65537"_bigint, "7716818999704200055673002605512017774829533873852931754420182187116755406508851421710377874835807810150544004124020368281638431187393087109588616395722976"_bigint);
    EXPECT_EQ(result0, "6957112022178657251467710742735822058162610570160374638904992058315050936014396238029779769209358140634220249380773356423403675888538086147825555026035553"_bigint);

    // 512 bits
    auto result1 = Crypto::NumberTheory::ModularInverse("65537"_bigint, "66371585251075966819781098993500728937583856843831372038905151148345437332287092304882812087499010029105588098364783005919549558874442528396629248591406931414614111891501372333038520092291512484438801203423887203269149674846124095871663987547448839320258336408613886916453844596419759100107324930878071769740"_bigint);
    EXPECT_EQ(result1, "26054622179142032720028508076442212084428946778480090764681215551421076128717366124902270573494164075542052047036494993565348604622774660543816175267575966621965870525545200512871843484053034799993241047965063186879250098185242452576259203314665246947408123972479812452501763277722372741633903726089081777013"_bigint);

    // 1024 bits
    auto result2 = Crypto::NumberTheory::ModularInverse("65537"_bigint, "15138018815872997670379340569590053786751606702300795170195880218956355437896550248537760818855924336022497803648355813501714375226639621651553768492566347398869904156530722997508431839019744455406614130583767126628559642684420295498410584657359791127851130600248257172505371207271304207113156882020325681053619922800978652053485848563399633561547330701503189380714480104549363705442836720246845910476607566548831148092234175836086100548136352482086041752239158391127234701836987492763766422215181929557528346258876471603164358341122158423252911442143627060117356562382539931055979839928020375814577774568506219095460"_bigint);
    EXPECT_EQ(result2, "352944027811067647898738611629058427852304118911692860827613485123904223707309287574434266615985662838432895066522539680342700540859443396609154496797860427323087928211223350781892424890095206186754144857591836206851688878370908212484113910561145014928308094010701389437847432819789627667865537264858898647327940583790765221748422671237234540519772362358619915066782513690761367501055197957446641610208834119453346877106578279102485033455183279561583102635479714079717024343606159710438913791366678187343078155600092293050263813498247677964057687773249647494687288513671987040199233950440440274115001289968681855713"_bigint);

    // 2048 bits
    auto result3 = Crypto::NumberTheory::ModularInverse("65537"_bigint, "523474308603627394504956180621539730601163404544670078344572546811775850669696720017356530287979625576623354887741212994543899068001220583437221973327752079153585098984263865181019654102487287512742287583901185619943683690635892036920956164864785078974721208937251159192154678447234191958275430233568974368064153896258338157469723619961352235804796084551641896006827645045906990423304676288895690876254935487456610269572418962650650646690483258846109000171328266193988292013017586921119096421585767248613790649741313360067618201749482055683058067852760706162692126354831896695191672470846960268467251962491660154005556677209860743434696351971155125630603082354855591129257818487022326288868392996237441507506020729258165681956915422119008555908702541877086255318047295376505201886687588318922810022094799926224262663342802397393873785019139429897232975310359190270883355499980682538341383918065122655507451050546937038544941011313947405743092260202204107637846238518077467613057097554476001838993189751185435880317537273891467684330982378878693444450893688310488368914140946077563025119239896138217432169087237109636595561779480434253413579986644072788364909696328314076474006110809917696250643811113150325166438321806889977329096600"_bigint);
    EXPECT_EQ(result3, "240127075385672984131139625830070783237907982221133353148189335410568341527428666156244401941613614961167400369106979053892812269120049657443477793981296225881475026790422579290126094592109424058098042199594448071964950528580600611958965243821505925343196113711042336371725072831518096843639993577853488509194999139161304606985554742922290191265996073819003163398587965470117671744141606775913928846496667921317852122223410154174992910744403897198385261335591218191096175027653809536744181084305551380061284286787205754668550681282247875856383030865885608272716379977803550823924611280514398989134855055135065370857211199581305881103457229188227055584369447256267812626743332730752890660577238791001818881550170150963398307775313919391546061252167851998883746488646960356804185182713413302894188591089552011567206439844281374992020196210238318522369271354430754186391905586095171569497490344824935263935189296620116395162680037583825943495347400986600883286030356418038099224122793594156156724989735012128839569555916857118867097884284041934024459778861054849599643478734444083949177169533378055193717492397723564200451231728283569509748271283984325804303130753631049728871294775611922359924670108389072405289815451858958044897456873"_bigint);

    // Prime modulus
    // 256 bits
    auto result4 = Crypto::NumberTheory::ModularInverse("79065576377430658630291493727884901955697921969202460485568061955796483998089"_bigint, "105236333148230907525852233540677623156492475210517338560791379084799836582587"_bigint);
    EXPECT_EQ(result4, "93504545219772953643321957341999793447107631393924073671776287172945600034443"_bigint);

    // 512 bits
    auto result5 = Crypto::NumberTheory::ModularInverse("6732413992718219635342848318074302303731222168385940253721776224551974038416513462421454674844777721589563127965274488341922551419528552939608455047714128"_bigint, "11522413189509252702442551731783393581283708206969207645140596867187940532466129960582867971721932546048110673296094625661627355203044884987258434322393611"_bigint);
    EXPECT_EQ(result5, "11152730475146621030888388443393672975086889576414759677260744095766476531703359323453287638858041043666073703397243706949753685433502205695232485731849432"_bigint);

    // 1024 bits
    auto result6 = Crypto::NumberTheory::ModularInverse("74263833960189886466939196560269216955870235656416128238251461763825971916420974189969964837983352188966833052749715539825280552531258436173317484112004327881741531787519471213020298642984697548930887036556763982001107471012474873100069623257613164741565312643996566523133343615723683010756027848816042939202"_bigint, "95381964444589883427387341140753255405844325814158762996484790475715776875097467150855290612578232487289384615394165716659709100194630793773552674979686871441395261056953751419334210618336786252840280983695277648363095334709545375311967459037971278965116324165577308183006400447807648095049414919774916252747"_bigint);
    EXPECT_EQ(result6, "58709722343881170435829301168583511620090591717154752336044125040931850388422639576614097557227300205781894345595418512100748823628637201919915110093901598005111776632116568475789059078360021536835127742733773460624284681421890935681567846755324337116900649074136799388542272888156479298282951539364264931616"_bigint);

    // 2048 bits
    auto result7 = Crypto::NumberTheory::ModularInverse("2083841562885492721290501151318058444158766003544222347122338319668970762119890042933475358898503059392439888781978346524976708635055122364241675726844930777696927712106305827918390408155067866218977660488635746552929258625544335318963328074495878439935663659069731717795216882935427203069231010795298950025561648743468756200717796561939220399337004980456668273620158478615916791124020696059432601192990947530965055857904582283829896086691653209249081553530465663724181700972927069397922147671340499270418643905380501155480764913403727582416414800901222394379992981688837765818280499497151738855424231982306618396076"_bigint, "16224364484369166277359386410182421629585266346687261081219199035627872465058014536404366328330233633748201670077151313307023144281234188494904998208639551259034363175330775169605905250528606169313713885192955997968412296964554695990505670926075345389730833276243454625387707778469967380099142375244892915645788614606443180803179195164798643205708829861402784554710221097157040790522116753790155662203858533778060827797234218324190122635514071740918420043227885163450453517325211468174509897086842869675754300089020572195273927710496253921910012981005407132203227555676309198192189264516679445448908377225879137304001"_bigint);
    EXPECT_EQ(result7, "1920241917211855356722925925154440229550377096185083909958775862353126205660695403426655365321463320876264364542077391170885582314150929024605918556565268345499952616868512453484734433431514794042936426911598410457811519189984561227978039512706300456181926682048163061548216104149539350320019907684566461197120360812572564919099529762677479436223515410468281993579286727653390573176288887687204943283770190210493492026862067176323654605190038514894818679839404911730667301011930597975461644362994301634764766641419232360033891763076329125623575026815152128746383453332269905123747535275999442797020400268408062413004"_bigint);
}

TEST_CASE(test_bigint_even_simple_modular_power)
{
    Crypto::UnsignedBigInteger base { 7 };
    Crypto::UnsignedBigInteger exponent { 2 };
    Crypto::UnsignedBigInteger modulo { 10 };
    auto result = Crypto::NumberTheory::ModularPower(base, exponent, modulo);
    EXPECT_EQ(result.words(), Vector<u32> { 9 });
}

TEST_CASE(test_bigint_odd_simple_modular_power)
{
    Crypto::UnsignedBigInteger base { 10 };
    Crypto::UnsignedBigInteger exponent { 2 };
    Crypto::UnsignedBigInteger modulo { 9 };
    auto result = Crypto::NumberTheory::ModularPower(base, exponent, modulo);
    EXPECT_EQ(result.words(), Vector<u32> { 1 });
}

TEST_CASE(test_bigint_large_even_fibonacci_modular_power)
{
    Crypto::UnsignedBigInteger base = bigint_fibonacci(200);
    Crypto::UnsignedBigInteger exponent = bigint_fibonacci(100);
    Crypto::UnsignedBigInteger modulo = bigint_fibonacci(150);
    // Result according to Wolfram Alpha : 7195284628716783672927396027925
    auto result = Crypto::NumberTheory::ModularPower(base, exponent, modulo);
    Vector<u32> expected_result { 2042093077, 1351416233, 3510104665, 90 };
    EXPECT_EQ(result.words(), expected_result);
}

TEST_CASE(test_bigint_large_odd_fibonacci_modular_power)
{
    Crypto::UnsignedBigInteger base = bigint_fibonacci(200);
    Crypto::UnsignedBigInteger exponent = bigint_fibonacci(100);
    Crypto::UnsignedBigInteger modulo = bigint_fibonacci(149);
    // Result according to Wolfram Alpha : 1136278609611966596838389694992
    auto result = Crypto::NumberTheory::ModularPower(base, exponent, modulo);
    Vector<u32> expected_result { 2106049040, 2169509253, 1468244710, 14 };
    EXPECT_EQ(result.words(), expected_result);
}

TEST_CASE(test_bigint_large_odd_fibonacci_with_carry_modular_power)
{
    Crypto::UnsignedBigInteger base = bigint_fibonacci(200);
    Crypto::UnsignedBigInteger exponent = bigint_fibonacci(100);
    Crypto::UnsignedBigInteger modulo = bigint_fibonacci(185);
    // Result according to Wolfram Alpha : 55094573983071006678665780782730672080
    auto result = Crypto::NumberTheory::ModularPower(base, exponent, modulo);
    Vector<u32> expected_result { 1988720592, 2097784252, 347129583, 695391288 };
    EXPECT_EQ(result.words(), expected_result);
}

TEST_CASE(test_bigint_modular_power_extra_tests)
{
    struct {
        Crypto::UnsignedBigInteger base;
        Crypto::UnsignedBigInteger exp;
        Crypto::UnsignedBigInteger mod;
        Crypto::UnsignedBigInteger expected;
    } mod_pow_tests[] = {
        { "2988348162058574136915891421498819466320163312926952423791023078876139"_bigint, "2351399303373464486466122544523690094744975233415544072992656881240319"_bigint, "10000"_bigint, "3059"_bigint },
        { "24231"_bigint, "12448"_bigint, "14679"_bigint, "4428"_bigint },
        { "1005404"_bigint, "8352654"_bigint, "8161408"_bigint, "2605696"_bigint },
        { "3665005778"_bigint, "3244425589"_bigint, "565668506"_bigint, "524766494"_bigint },
        { "10662083169959689657"_bigint, "11605678468317533000"_bigint, "1896834583057209739"_bigint, "1292743154593945858"_bigint },
        { "99667739213529524852296932424683448520"_bigint, "123394910770101395416306279070921784207"_bigint, "238026722756504133786938677233768788719"_bigint, "197165477545023317459748215952393063201"_bigint },
        { "49368547511968178788919424448914214709244872098814465088945281575062739912239"_bigint, "25201856190991298572337188495596990852134236115562183449699512394891190792064"_bigint, "45950460777961491021589776911422805972195170308651734432277141467904883064645"_bigint, "39917885806532796066922509794537889114718612292469285403012781055544152450051"_bigint },
        { "48399385336454791246880286907257136254351739111892925951016159217090949616810"_bigint, "5758661760571644379364752528081901787573279669668889744323710906207949658569"_bigint, "32812120644405991429173950312949738783216437173380339653152625840449006970808"_bigint, "7948464125034399875323770213514649646309423451213282653637296324080400293584"_bigint },
    };

    for (auto test_case : mod_pow_tests) {
        auto actual = Crypto::NumberTheory::ModularPower(
            test_case.base, test_case.exp, test_case.mod);

        EXPECT_EQ(actual, test_case.expected);
    }
}

TEST_CASE(test_bigint_primality_test)
{
    struct {
        Crypto::UnsignedBigInteger candidate;
        bool expected_result;
    } primality_tests[] = {
        { "1180591620717411303424"_bigint, false },                  // 2**70
        { "620448401733239439360000"_bigint, false },                // 25!
        { "953962166440690129601298432"_bigint, false },             // 12**25
        { "620448401733239439360000"_bigint, false },                // 25!
        { "147926426347074375"_bigint, false },                      // 35! / 2**32
        { "340282366920938429742726440690708343523"_bigint, false }, // 2 factors near 2^64
        { "73"_bigint, true },
        { "6967"_bigint, true },
        { "787649"_bigint, true },
        { "73513949"_bigint, true },
        { "6691236901"_bigint, true },
        { "741387182759"_bigint, true },
        { "67466615915827"_bigint, true },
        { "9554317039214687"_bigint, true },
        { "533344522150170391"_bigint, true },
        { "18446744073709551557"_bigint, true }, // just below 2**64
    };

    for (auto test_case : primality_tests) {
        bool actual_result = Crypto::NumberTheory::is_probably_prime(test_case.candidate);
        EXPECT_EQ(test_case.expected_result, actual_result);
    }
}

TEST_CASE(test_bigint_random_number_generation)
{
    struct {
        Crypto::UnsignedBigInteger min;
        Crypto::UnsignedBigInteger max;
    } random_number_tests[] = {
        { "1"_bigint, "1000000"_bigint },
        { "10000000000"_bigint, "20000000000"_bigint },
        { "1000"_bigint, "200000000000000000"_bigint },
        { "200000000000000000"_bigint, "200000000000010000"_bigint },
    };

    for (auto test_case : random_number_tests) {
        auto actual_result = Crypto::NumberTheory::random_number(test_case.min, test_case.max);
        EXPECT(!(actual_result < test_case.min));
        EXPECT(actual_result < test_case.max);
    }
}

TEST_CASE(test_bigint_random_distribution)
{
    auto actual_result = Crypto::NumberTheory::random_number(
        "1"_bigint,
        "100000000000000000000000000000"_bigint);         // 10**29
    if (actual_result < "100000000000000000000"_bigint) { // 10**20
        FAIL("Too small");
        outln("The generated number {} is extremely small. This *can* happen by pure chance, but should happen only once in a billion times. So it's probably an error.", MUST(actual_result.to_base(10)));
    } else if ("99999999900000000000000000000"_bigint < actual_result) { // 10**29 - 10**20
        FAIL("Too large");
        outln("The generated number {} is extremely large. This *can* happen by pure chance, but should happen only once in a billion times. So it's probably an error.", MUST(actual_result.to_base(10)));
    }
}

TEST_CASE(test_bigint_import_big_endian_decode_encode_roundtrip)
{
    u8 random_bytes[128];
    u8 target_buffer[128];
    fill_with_random(random_bytes);
    auto encoded = Crypto::UnsignedBigInteger::import_data(random_bytes, 128);
    encoded.export_data({ target_buffer, 128 });
    EXPECT(memcmp(target_buffer, random_bytes, 128) == 0);
}

TEST_CASE(test_bigint_import_big_endian_encode_decode_roundtrip)
{
    u8 target_buffer[128];
    auto encoded = "12345678901234567890"_bigint;
    auto size = encoded.export_data({ target_buffer, 128 });
    auto decoded = Crypto::UnsignedBigInteger::import_data(target_buffer, size);
    EXPECT_EQ(encoded, decoded);
}

TEST_CASE(test_bigint_big_endian_import)
{
    auto number = Crypto::UnsignedBigInteger::import_data("hello"sv);
    EXPECT_EQ(number, "448378203247"_bigint);
}

TEST_CASE(test_bigint_big_endian_export)
{
    auto number = "448378203247"_bigint;
    char exported[8] { 0 };
    auto exported_length = number.export_data({ exported, 8 }, true);
    EXPECT_EQ(exported_length, 5u);
    EXPECT(memcmp(exported + 3, "hello", 5) == 0);
}

TEST_CASE(test_bigint_one_based_index_of_highest_set_bit)
{
    auto num1 = "1234567"_bigint;
    auto num2 = "1234567"_bigint;
    EXPECT_EQ("0"_bigint.one_based_index_of_highest_set_bit(), 0u);
    EXPECT_EQ("1"_bigint.one_based_index_of_highest_set_bit(), 1u);
    EXPECT_EQ("7"_bigint.one_based_index_of_highest_set_bit(), 3u);
    EXPECT_EQ("4294967296"_bigint.one_based_index_of_highest_set_bit(), 33u);
}

TEST_CASE(test_signed_bigint_bitwise_not_fill_to_one_based_index)
{
    EXPECT_EQ("0"_bigint.bitwise_not_fill_to_one_based_index(0), "0"_bigint);
    EXPECT_EQ("0"_bigint.bitwise_not_fill_to_one_based_index(1), "1"_bigint);
    EXPECT_EQ("0"_bigint.bitwise_not_fill_to_one_based_index(2), "3"_bigint);
    EXPECT_EQ("0"_bigint.bitwise_not_fill_to_one_based_index(4), "15"_bigint);
    EXPECT_EQ("0"_bigint.bitwise_not_fill_to_one_based_index(32), "4294967295"_bigint);
    EXPECT_EQ("0"_bigint.bitwise_not_fill_to_one_based_index(33), "8589934591"_bigint);
}

TEST_CASE(test_bigint_bitwise_or)
{
    auto num1 = "1234567"_bigint;
    auto num2 = "1234567"_bigint;
    EXPECT_EQ(num1.bitwise_or(num2), num1);
}

TEST_CASE(test_bigint_bitwise_or_different_lengths)
{
    auto num1 = "1234567"_bigint;
    auto num2 = "123456789012345678901234567890"_bigint;
    auto expected = "123456789012345678901234622167"_bigint;
    auto result = num1.bitwise_or(num2);
    EXPECT_EQ(result, expected);
}

TEST_CASE(test_signed_bigint_bitwise_or)
{
    auto num1 = "-1234567"_sbigint;
    auto num2 = "1234567"_sbigint;
    EXPECT_EQ(num1.bitwise_or(num1), num1);
    EXPECT_EQ(num1.bitwise_or(num2), "-1"_sbigint);
    EXPECT_EQ(num2.bitwise_or(num1), "-1"_sbigint);
    EXPECT_EQ(num2.bitwise_or(num2), num2);

    EXPECT_EQ("0"_sbigint.bitwise_or("-1"_sbigint), "-1"_sbigint);
}

TEST_CASE(test_bigint_bitwise_and)
{
    auto num1 = "1234567"_bigint;
    auto num2 = "1234561"_bigint;
    EXPECT_EQ(num1.bitwise_and(num2), "1234561"_bigint);
}

TEST_CASE(test_bigint_bitwise_and_different_lengths)
{
    auto num1 = "1234567"_bigint;
    auto num2 = "123456789012345678901234567890"_bigint;
    EXPECT_EQ(num1.bitwise_and(num2), "1180290"_bigint);
}

TEST_CASE(test_signed_bigint_bitwise_not)
{
    EXPECT_EQ("3"_sbigint.bitwise_not(), "-4"_sbigint);
    EXPECT_EQ("-1"_sbigint.bitwise_not(), "0"_sbigint);
}

TEST_CASE(test_signed_bigint_bitwise_and)
{
    auto num1 = "-1234567"_sbigint;
    auto num2 = "1234567"_sbigint;
    EXPECT_EQ(num1.bitwise_and(num1), num1);
    EXPECT_EQ(num1.bitwise_and(num2), "1"_sbigint);
    EXPECT_EQ(num2.bitwise_and(num1), "1"_sbigint);
    EXPECT_EQ(num2.bitwise_and(num2), num2);

    EXPECT_EQ("-3"_sbigint.bitwise_and("-2"_sbigint), "-4"_sbigint);
}

TEST_CASE(test_bigint_bitwise_xor)
{
    auto num1 = "1234567"_bigint;
    auto num2 = "1234561"_bigint;
    EXPECT_EQ(num1.bitwise_xor(num2), 6);
}

TEST_CASE(test_bigint_bitwise_xor_different_lengths)
{
    auto num1 = "1234567"_bigint;
    auto num2 = "123456789012345678901234567890"_bigint;
    EXPECT_EQ(num1.bitwise_xor(num2), "123456789012345678901233441877"_bigint);
}

TEST_CASE(test_signed_bigint_bitwise_xor)
{
    auto num1 = "-3"_sbigint;
    auto num2 = "1"_sbigint;
    EXPECT_EQ(num1.bitwise_xor(num1), "0"_sbigint);
    EXPECT_EQ(num1.bitwise_xor(num2), "-4"_sbigint);
    EXPECT_EQ(num2.bitwise_xor(num1), "-4"_sbigint);
    EXPECT_EQ(num2.bitwise_xor(num2), "0"_sbigint);
}

TEST_CASE(test_bigint_shift_left)
{
    Crypto::UnsignedBigInteger const num(Vector<u32> { 0x22222222, 0xffffffff });

    size_t const tests = 8;
    AK::Tuple<size_t, Vector<u32>> results[] = {
        { 0, { 0x22222222, 0xffffffff } },
        { 8, { 0x22222200, 0xffffff22, 0x000000ff } },
        { 16, { 0x22220000, 0xffff2222, 0x0000ffff } },
        { 32, { 0x00000000, 0x22222222, 0xffffffff } },
        { 36, { 0x00000000, 0x22222220, 0xfffffff2, 0x0000000f } },
        { 40, { 0x00000000, 0x22222200, 0xffffff22, 0x000000ff } },
        { 64, { 0x00000000, 0x00000000, 0x22222222, 0xffffffff } },
        { 68, { 0x00000000, 0x00000000, 0x22222220, 0xfffffff2, 0x0000000f } },
    };

    for (size_t i = 0; i < tests; ++i)
        EXPECT_EQ(num.shift_left(results[i].get<0>()).words(), results[i].get<1>());
}

TEST_CASE(test_bigint_shift_right)
{
    Crypto::UnsignedBigInteger const num1(Vector<u32> { 0x100, 0x20, 0x4, 0x2, 0x1 });

    size_t const tests1 = 11;
    AK::Tuple<size_t, Vector<u32>> results1[] = {
        { 8, { 0x20000001, 0x04000000, 0x02000000, 0x01000000 } },
        { 16, { 0x00200000, 0x00040000, 0x00020000, 0x00010000 } }, // shift by exact number of words
        { 32, { 0x00000020, 0x00000004, 0x00000002, 0x00000001 } }, // shift by exact number of words
        { 36, { 0x40000002, 0x20000000, 0x10000000 } },
        { 64, { 0x00000004, 0x00000002, 0x00000001 } }, // shift by exact number of words
        { 72, { 0x02000000, 0x01000000 } },
        { 80, { 0x00020000, 0x00010000 } },
        { 88, { 0x00000200, 0x00000100 } },
        { 128, { 0x00000001 } }, // shifted to most significant digit
        { 129, {} },             // all digits have been shifted right
        { 160, {} },
    };

    size_t const tests2 = 2;
    Crypto::UnsignedBigInteger const num2(Vector<u32> { 0x44444444, 0xffffffff });

    AK::Tuple<size_t, Vector<u32>> results2[] = {
        { 1, { 0xa2222222, 0x7fffffff } },
        { 2, { 0xd1111111, 0x3fffffff } },
    };

    for (size_t i = 0; i < tests1; ++i)
        EXPECT_EQ(num1.shift_right(results1[i].get<0>()).words(), results1[i].get<1>());

    for (size_t i = 0; i < tests2; ++i)
        EXPECT_EQ(num2.shift_right(results2[i].get<0>()).words(), results2[i].get<1>());
}

TEST_CASE(test_signed_bigint_fibo500)
{
    Vector<u32> expected_result {
        315178285, 505575602, 1883328078, 125027121,
        3649625763, 347570207, 74535262, 3832543808,
        2472133297, 1600064941, 65273441
    };
    auto result = bigint_signed_fibonacci(500);
    EXPECT_EQ(result.unsigned_value().words(), expected_result);
}

TEST_CASE(test_signed_addition_edgecase_borrow_with_zero)
{
    Crypto::SignedBigInteger num1 { Crypto::UnsignedBigInteger { { UINT32_MAX - 3, UINT32_MAX } }, false };
    Crypto::SignedBigInteger num2 { Crypto::UnsignedBigInteger { UINT32_MAX - 2 }, false };
    Vector<u32> expected_result { 4294967289, 0, 1 };
    EXPECT_EQ(num1.plus(num2).unsigned_value().words(), expected_result);
}

TEST_CASE(test_signed_addition_edgecase_addition_to_other_sign)
{
    Crypto::SignedBigInteger num1 = INT32_MAX;
    Crypto::SignedBigInteger num2 = num1;
    num2.negate();
    EXPECT_EQ(num1.plus(num2), Crypto::SignedBigInteger { 0 });
}

TEST_CASE(test_signed_subtraction_simple_subtraction_positive_result)
{
    Crypto::SignedBigInteger num1(80);
    Crypto::SignedBigInteger num2(70);
    EXPECT_EQ(num1.minus(num2), Crypto::SignedBigInteger(10));
}

TEST_CASE(test_signed_subtraction_simple_subtraction_negative_result)
{
    Crypto::SignedBigInteger num1(50);
    Crypto::SignedBigInteger num2(70);

    EXPECT_EQ(num1.minus(num2), Crypto::SignedBigInteger { -20 });
}

TEST_CASE(test_signed_subtraction_both_negative)
{
    Crypto::SignedBigInteger num1(-50);
    Crypto::SignedBigInteger num2(-70);

    EXPECT_EQ(num1.minus(num2), Crypto::SignedBigInteger { 20 });
    EXPECT_EQ(num2.minus(num1), Crypto::SignedBigInteger { -20 });
}

TEST_CASE(test_signed_subtraction_simple_subtraction_with_borrow)
{
    Crypto::SignedBigInteger num1(Crypto::UnsignedBigInteger { UINT32_MAX });
    Crypto::SignedBigInteger num2(1);
    Crypto::SignedBigInteger num3 = num1.plus(num2);
    Crypto::SignedBigInteger result = num2.minus(num3);
    num1.negate();
    EXPECT_EQ(result, num1);
}

TEST_CASE(test_signed_subtraction_with_large_numbers)
{
    Crypto::SignedBigInteger num1 = bigint_signed_fibonacci(343);
    Crypto::SignedBigInteger num2 = bigint_signed_fibonacci(218);
    Crypto::SignedBigInteger result = num2.minus(num1);
    auto expected = Crypto::UnsignedBigInteger { Vector<u32> { 811430588, 2958904896, 1130908877, 2830569969, 3243275482, 3047460725, 774025231, 7990 } };
    EXPECT_EQ(result.plus(num1), num2);
    EXPECT_EQ(result.unsigned_value(), expected);
}

TEST_CASE(test_signed_subtraction_with_large_numbers_check_for_assertion)
{
    Crypto::SignedBigInteger num1(Crypto::UnsignedBigInteger { Vector<u32> { 1483061863, 446680044, 1123294122, 191895498, 3347106536, 16, 0, 0, 0 } });
    Crypto::SignedBigInteger num2(Crypto::UnsignedBigInteger { Vector<u32> { 4196414175, 1117247942, 1123294122, 191895498, 3347106536, 16 } });
    Crypto::SignedBigInteger result = num1.minus(num2);
    // this test only verifies that we don't crash on an assertion
}

TEST_CASE(test_signed_multiplication_with_negative_number)
{
    Crypto::SignedBigInteger num1(8);
    Crypto::SignedBigInteger num2(-251);
    Crypto::SignedBigInteger result = num1.multiplied_by(num2);
    EXPECT_EQ(result, Crypto::SignedBigInteger { -2008 });
}

TEST_CASE(test_signed_multiplication_with_big_number)
{
    Crypto::SignedBigInteger num1 = bigint_signed_fibonacci(200);
    Crypto::SignedBigInteger num2(-12345678);
    Crypto::SignedBigInteger result = num1.multiplied_by(num2);
    Vector<u32> expected_result { 669961318, 143970113, 4028714974, 3164551305, 1589380278, 2 };
    EXPECT_EQ(result.unsigned_value().words(), expected_result);
    EXPECT(result.is_negative());
}

TEST_CASE(test_signed_multiplication_with_two_big_numbers)
{
    Crypto::SignedBigInteger num1 = bigint_signed_fibonacci(200);
    Crypto::SignedBigInteger num2 = bigint_signed_fibonacci(341);
    num1.negate();
    Crypto::SignedBigInteger result = num1.multiplied_by(num2);
    Vector<u32> expected_results {
        3017415433, 2741793511, 1957755698, 3731653885,
        3154681877, 785762127, 3200178098, 4260616581,
        529754471, 3632684436, 1073347813, 2516430
    };
    EXPECT_EQ(result.unsigned_value().words(), expected_results);
    EXPECT(result.is_negative());
}

TEST_CASE(test_negative_zero_is_not_allowed)
{
    Crypto::SignedBigInteger zero(Crypto::UnsignedBigInteger(0), true);
    EXPECT(!zero.is_negative());

    zero.negate();
    EXPECT(!zero.is_negative());

    Crypto::SignedBigInteger positive_five(Crypto::UnsignedBigInteger(5), false);
    Crypto::SignedBigInteger negative_five(Crypto::UnsignedBigInteger(5), true);
    zero = positive_five.plus(negative_five);

    EXPECT(zero.unsigned_value().is_zero());
    EXPECT(!zero.is_negative());
}

TEST_CASE(test_i32_limits)
{
    Crypto::SignedBigInteger min { AK::NumericLimits<i32>::min() };
    EXPECT(min.is_negative());
    EXPECT(min.unsigned_value().to_u64() == static_cast<u32>(AK::NumericLimits<i32>::max()) + 1);

    Crypto::SignedBigInteger max { AK::NumericLimits<i32>::max() };
    EXPECT(!max.is_negative());
    EXPECT(max.unsigned_value().to_u64() == AK::NumericLimits<i32>::max());
}

TEST_CASE(double_comparisons)
{
#define EXPECT_LESS_THAN(bigint, double_value) EXPECT_EQ(bigint.compare_to_double(double_value), Crypto::UnsignedBigInteger::CompareResult::DoubleGreaterThanBigInt)
#define EXPECT_GREATER_THAN(bigint, double_value) EXPECT_EQ(bigint.compare_to_double(double_value), Crypto::UnsignedBigInteger::CompareResult::DoubleLessThanBigInt)
#define EXPECT_EQUAL_TO(bigint, double_value) EXPECT_EQ(bigint.compare_to_double(double_value), Crypto::UnsignedBigInteger::CompareResult::DoubleEqualsBigInt)
    {
        Crypto::SignedBigInteger zero { 0 };
        EXPECT_EQUAL_TO(zero, 0.0);
        EXPECT_EQUAL_TO(zero, -0.0);
    }

    {
        Crypto::SignedBigInteger one { 1 };
        EXPECT_EQUAL_TO(one, 1.0);
        EXPECT_GREATER_THAN(one, -1.0);
        EXPECT_GREATER_THAN(one, 0.5);
        EXPECT_GREATER_THAN(one, -0.5);
        EXPECT_LESS_THAN(one, 1.000001);

        one.negate();
        auto const& negative_one = one;
        EXPECT_EQUAL_TO(negative_one, -1.0);
        EXPECT_LESS_THAN(negative_one, 1.0);
        EXPECT_LESS_THAN(one, 0.5);
        EXPECT_LESS_THAN(one, -0.5);
        EXPECT_GREATER_THAN(one, -1.5);
        EXPECT_LESS_THAN(one, 1.000001);
        EXPECT_GREATER_THAN(one, -1.000001);
    }

    {
        double double_infinity = HUGE_VAL;
        VERIFY(isinf(double_infinity));
        Crypto::SignedBigInteger one { 1 };
        EXPECT_LESS_THAN(one, double_infinity);
        EXPECT_GREATER_THAN(one, -double_infinity);
    }

    {
        double double_max_value = NumericLimits<double>::max();
        double double_below_max_value = nextafter(double_max_value, 0.0);
        VERIFY(double_below_max_value < double_max_value);
        VERIFY(double_below_max_value < (double_max_value - 1.0));
        auto max_value_in_bigint = TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "fffffffffffff800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv));
        auto max_value_plus_one = max_value_in_bigint.plus(Crypto::SignedBigInteger { 1 });
        auto max_value_minus_one = max_value_in_bigint.minus(Crypto::SignedBigInteger { 1 });

        auto below_max_value_in_bigint = TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "fffffffffffff000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv));

        EXPECT_EQUAL_TO(max_value_in_bigint, double_max_value);
        EXPECT_LESS_THAN(max_value_minus_one, double_max_value);
        EXPECT_GREATER_THAN(max_value_plus_one, double_max_value);
        EXPECT_LESS_THAN(below_max_value_in_bigint, double_max_value);

        EXPECT_GREATER_THAN(max_value_in_bigint, double_below_max_value);
        EXPECT_GREATER_THAN(max_value_minus_one, double_below_max_value);
        EXPECT_GREATER_THAN(max_value_plus_one, double_below_max_value);
        EXPECT_EQUAL_TO(below_max_value_in_bigint, double_below_max_value);
    }

    {
        double double_min_value = NumericLimits<double>::lowest();
        double double_above_min_value = nextafter(double_min_value, 0.0);
        VERIFY(double_above_min_value > double_min_value);
        VERIFY(double_above_min_value > (double_min_value + 1.0));
        auto min_value_in_bigint = TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "-fffffffffffff800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv));
        auto min_value_plus_one = min_value_in_bigint.plus(Crypto::SignedBigInteger { 1 });
        auto min_value_minus_one = min_value_in_bigint.minus(Crypto::SignedBigInteger { 1 });

        auto above_min_value_in_bigint = TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "-fffffffffffff000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv));

        EXPECT_EQUAL_TO(min_value_in_bigint, double_min_value);
        EXPECT_LESS_THAN(min_value_minus_one, double_min_value);
        EXPECT_GREATER_THAN(min_value_plus_one, double_min_value);
        EXPECT_GREATER_THAN(above_min_value_in_bigint, double_min_value);

        EXPECT_LESS_THAN(min_value_in_bigint, double_above_min_value);
        EXPECT_LESS_THAN(min_value_minus_one, double_above_min_value);
        EXPECT_LESS_THAN(min_value_plus_one, double_above_min_value);
        EXPECT_EQUAL_TO(above_min_value_in_bigint, double_above_min_value);
    }

    {
        double just_above_255 = bit_cast<double>(0x406fe00000000001ULL);
        double just_below_255 = bit_cast<double>(0x406fdfffffffffffULL);
        double double_255 = 255.0;
        Crypto::SignedBigInteger bigint_255 { 255 };

        EXPECT_EQUAL_TO(bigint_255, double_255);
        EXPECT_GREATER_THAN(bigint_255, just_below_255);
        EXPECT_LESS_THAN(bigint_255, just_above_255);
    }

#undef EXPECT_LESS_THAN
#undef EXPECT_GREATER_THAN
#undef EXPECT_EQUAL_TO
}

TEST_CASE(to_double)
{
#define EXPECT_TO_EQUAL_DOUBLE(bigint, double_value) \
    EXPECT_EQ((bigint).to_double(Crypto::UnsignedBigInteger::RoundingMode::RoundTowardZero), double_value)

    EXPECT_TO_EQUAL_DOUBLE(Crypto::UnsignedBigInteger(0), 0.0);
    // Make sure we don't get negative zero!
    EXPECT_EQ(signbit(Crypto::UnsignedBigInteger(0).to_double()), 0);
    {
        Crypto::SignedBigInteger zero { 0 };

        EXPECT(!zero.is_negative());
        EXPECT_TO_EQUAL_DOUBLE(zero, 0.0);
        EXPECT_EQ(signbit(zero.to_double()), 0);

        zero.negate();

        EXPECT(!zero.is_negative());
        EXPECT_TO_EQUAL_DOUBLE(zero, 0.0);
        EXPECT_EQ(signbit(zero.to_double()), 0);
    }

    EXPECT_TO_EQUAL_DOUBLE(Crypto::UnsignedBigInteger(9682), 9682.0);
    EXPECT_TO_EQUAL_DOUBLE(Crypto::SignedBigInteger(-9660), -9660.0);

    double double_max_value = NumericLimits<double>::max();
    double infinity = INFINITY;

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "fffffffffffff800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv)),
        double_max_value);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "ffffffffffffff00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv)),
        double_max_value);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"sv)),
        double_max_value);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv)),
        infinity);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "-fffffffffffff800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv)),
        -double_max_value);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "-ffffffffffffff00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv)),
        -double_max_value);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "-ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"sv)),
        -double_max_value);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(16, "-10000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv)),
        -infinity);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "ffffffffffffffff"sv)),
        18446744073709549568.0);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "fffffffffffff800"sv)),
        18446744073709549568.0);

    EXPECT_TO_EQUAL_DOUBLE(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "fffffffffffff8ff"sv)),
        18446744073709549568.0);

    EXPECT_TO_EQUAL_DOUBLE(TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(10, "1234567890123456789"sv)),
        1234567890123456800.0);

    EXPECT_TO_EQUAL_DOUBLE(TRY_OR_FAIL(Crypto::SignedBigInteger::from_base(10, "2345678901234567890"sv)),
        2345678901234567680.0);

    EXPECT_EQ(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "1fffffffffffff00"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        2305843009213693696.0);

    EXPECT_EQ(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "1fffffffffffff00"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::RoundTowardZero),
        2305843009213693696.0);

    EXPECT_EQ(
        TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "1fffffffffffff80"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        2305843009213693952.0);

    EXPECT_EQ(TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "20000000000001"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        9007199254740992.0);

    EXPECT_EQ(TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "20000000000002"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        9007199254740994.0);

    // 2^53 = 20000000000000, +3 Rounds up because of tiesRoundToEven
    EXPECT_EQ(TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "20000000000003"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        9007199254740996.0);

    // +4 is exactly 9007199254740996
    EXPECT_EQ(TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "20000000000004"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        9007199254740996.0);

    // +5 rounds down because of tiesRoundToEven
    EXPECT_EQ(TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "20000000000005"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        9007199254740996.0);

    EXPECT_EQ(TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "20000000000006"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        9007199254740998.0);

    EXPECT_EQ(TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(10, "98382635059784269824"sv)).to_double(Crypto::UnsignedBigInteger::RoundingMode::IEEERoundAndTiesToEvenMantissa),
        bit_cast<double>(0x4415555555555555ULL));

#undef EXPECT_TO_EQUAL_DOUBLE
}

TEST_CASE(bigint_from_double)
{
    {
        Crypto::UnsignedBigInteger from_zero { 0.0 };
        EXPECT(from_zero.is_zero());
        EXPECT(!from_zero.is_invalid());
    }

#define SURVIVES_ROUND_TRIP_UNSIGNED(double_value)            \
    {                                                         \
        Crypto::UnsignedBigInteger bigint { (double_value) }; \
        EXPECT_EQ(bigint.to_double(), (double_value));        \
    }

    SURVIVES_ROUND_TRIP_UNSIGNED(0.0);
    SURVIVES_ROUND_TRIP_UNSIGNED(1.0);
    SURVIVES_ROUND_TRIP_UNSIGNED(100000.0);
    SURVIVES_ROUND_TRIP_UNSIGNED(1000000000000.0);
    SURVIVES_ROUND_TRIP_UNSIGNED(10000000000000000000.0);
    SURVIVES_ROUND_TRIP_UNSIGNED(NumericLimits<double>::max());

    SURVIVES_ROUND_TRIP_UNSIGNED(bit_cast<double>(0x4340000000000002ULL));
    SURVIVES_ROUND_TRIP_UNSIGNED(bit_cast<double>(0x4340000000000001ULL));
    SURVIVES_ROUND_TRIP_UNSIGNED(bit_cast<double>(0x4340000000000000ULL));

    // Failed on last bits of mantissa
    SURVIVES_ROUND_TRIP_UNSIGNED(bit_cast<double>(0x7EDFFFFFFFFFFFFFULL));
    SURVIVES_ROUND_TRIP_UNSIGNED(bit_cast<double>(0x7ed5555555555555ULL));
    SURVIVES_ROUND_TRIP_UNSIGNED(bit_cast<double>(0x7EDCBA9876543210ULL));

    // Has exactly exponent of 32
    SURVIVES_ROUND_TRIP_UNSIGNED(bit_cast<double>(0x41f22f74e0000000ULL));

#define SURVIVES_ROUND_TRIP_SIGNED(double_value)                      \
    {                                                                 \
        Crypto::SignedBigInteger bigint_positive { (double_value) };  \
        EXPECT_EQ(bigint_positive.to_double(), (double_value));       \
        Crypto::SignedBigInteger bigint_negative { -(double_value) }; \
        EXPECT_EQ(bigint_negative.to_double(), -(double_value));      \
        EXPECT(bigint_positive != bigint_negative);                   \
        bigint_positive.negate();                                     \
        EXPECT(bigint_positive == bigint_negative);                   \
    }

    {
        // Negative zero should be converted to positive zero
        double const negative_zero = bit_cast<double>(0x8000000000000000);

        // However it should give a bit exact +0.0
        Crypto::SignedBigInteger from_negative_zero { negative_zero };
        EXPECT(from_negative_zero.is_zero());
        EXPECT(!from_negative_zero.is_negative());
        double result = from_negative_zero.to_double();
        EXPECT_EQ(result, 0.0);
        EXPECT_EQ(bit_cast<u64>(result), 0ULL);
    }

    SURVIVES_ROUND_TRIP_SIGNED(1.0);
    SURVIVES_ROUND_TRIP_SIGNED(100000.0);
    SURVIVES_ROUND_TRIP_SIGNED(-1000000000000.0);
    SURVIVES_ROUND_TRIP_SIGNED(10000000000000000000.0);
    SURVIVES_ROUND_TRIP_SIGNED(NumericLimits<double>::max());
    SURVIVES_ROUND_TRIP_SIGNED(NumericLimits<double>::lowest());

    SURVIVES_ROUND_TRIP_SIGNED(bit_cast<double>(0x4340000000000002ULL));
    SURVIVES_ROUND_TRIP_SIGNED(bit_cast<double>(0x4340000000000001ULL));
    SURVIVES_ROUND_TRIP_SIGNED(bit_cast<double>(0x4340000000000000ULL));
    SURVIVES_ROUND_TRIP_SIGNED(bit_cast<double>(0x7EDFFFFFFFFFFFFFULL));
    SURVIVES_ROUND_TRIP_SIGNED(bit_cast<double>(0x7ed5555555555555ULL));
    SURVIVES_ROUND_TRIP_SIGNED(bit_cast<double>(0x7EDCBA9876543210ULL));

#undef SURVIVES_ROUND_TRIP_SIGNED
#undef SURVIVES_ROUND_TRIP_UNSIGNED
}

TEST_CASE(unsigned_bigint_double_comparisons)
{
#define EXPECT_LESS_THAN(bigint, double_value) EXPECT_EQ(bigint.compare_to_double(double_value), Crypto::UnsignedBigInteger::CompareResult::DoubleGreaterThanBigInt)
#define EXPECT_GREATER_THAN(bigint, double_value) EXPECT_EQ(bigint.compare_to_double(double_value), Crypto::UnsignedBigInteger::CompareResult::DoubleLessThanBigInt)
#define EXPECT_EQUAL_TO(bigint, double_value) EXPECT_EQ(bigint.compare_to_double(double_value), Crypto::UnsignedBigInteger::CompareResult::DoubleEqualsBigInt)

    {
        Crypto::UnsignedBigInteger zero { 0 };
        EXPECT_EQUAL_TO(zero, 0.0);
        EXPECT_EQUAL_TO(zero, -0.0);
    }

    {
        Crypto::UnsignedBigInteger one { 1 };
        EXPECT_EQUAL_TO(one, 1.0);
        EXPECT_GREATER_THAN(one, -1.0);
        EXPECT_GREATER_THAN(one, 0.5);
        EXPECT_GREATER_THAN(one, -0.5);
        EXPECT_LESS_THAN(one, 1.000001);
    }

    {
        double double_infinity = HUGE_VAL;
        VERIFY(isinf(double_infinity));
        Crypto::UnsignedBigInteger one { 1 };
        EXPECT_LESS_THAN(one, double_infinity);
        EXPECT_GREATER_THAN(one, -double_infinity);
    }

    {
        double double_max_value = NumericLimits<double>::max();
        double double_below_max_value = nextafter(double_max_value, 0.0);
        VERIFY(double_below_max_value < double_max_value);
        VERIFY(double_below_max_value < (double_max_value - 1.0));
        auto max_value_in_bigint = TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "fffffffffffff800000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv));
        auto max_value_plus_one = max_value_in_bigint.plus(Crypto::UnsignedBigInteger { 1 });
        auto max_value_minus_one = max_value_in_bigint.minus(Crypto::UnsignedBigInteger { 1 });

        auto below_max_value_in_bigint = TRY_OR_FAIL(Crypto::UnsignedBigInteger::from_base(16, "fffffffffffff000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"sv));

        EXPECT_EQUAL_TO(max_value_in_bigint, double_max_value);
        EXPECT_LESS_THAN(max_value_minus_one, double_max_value);
        EXPECT_GREATER_THAN(max_value_plus_one, double_max_value);
        EXPECT_LESS_THAN(below_max_value_in_bigint, double_max_value);

        EXPECT_GREATER_THAN(max_value_in_bigint, double_below_max_value);
        EXPECT_GREATER_THAN(max_value_minus_one, double_below_max_value);
        EXPECT_GREATER_THAN(max_value_plus_one, double_below_max_value);
        EXPECT_EQUAL_TO(below_max_value_in_bigint, double_below_max_value);
    }

    {
        double just_above_255 = bit_cast<double>(0x406fe00000000001ULL);
        double just_below_255 = bit_cast<double>(0x406fdfffffffffffULL);
        double double_255 = 255.0;
        Crypto::UnsignedBigInteger bigint_255 { 255 };

        EXPECT_EQUAL_TO(bigint_255, double_255);
        EXPECT_GREATER_THAN(bigint_255, just_below_255);
        EXPECT_LESS_THAN(bigint_255, just_above_255);
    }

#undef EXPECT_LESS_THAN
#undef EXPECT_GREATER_THAN
#undef EXPECT_EQUAL_TO
}

namespace AK {

template<>
struct Formatter<Crypto::UnsignedBigInteger::CompareResult> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Crypto::UnsignedBigInteger::CompareResult const& compare_result)
    {
        switch (compare_result) {
        case Crypto::UnsignedBigInteger::CompareResult::DoubleEqualsBigInt:
            return builder.put_string("Equals"sv);
        case Crypto::UnsignedBigInteger::CompareResult::DoubleLessThanBigInt:
            return builder.put_string("LessThan"sv);
        case Crypto::UnsignedBigInteger::CompareResult::DoubleGreaterThanBigInt:
            return builder.put_string("GreaterThan"sv);
        default:
            return builder.put_string("???"sv);
        }
    }
};

}
