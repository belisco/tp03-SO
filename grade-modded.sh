#!/bin/bash
set -u

TESTSPEC=mempager-tests/tests.spec
# TESTSPEC=mempager-tests/test11.spec

echo "=========================================="
echo "Compilando o projeto..."
echo "=========================================="
make

echo ""
echo "=========================================="
echo "Executando testes..."
echo "=========================================="
echo ""

PASSED=0
FAILED=0
TOTAL=0

while read -r num frames blocks nodiff ; do
    num=$((num))
    frames=$((frames))
    blocks=$((blocks))
    nodiff=$((nodiff))
    
    TOTAL=$((TOTAL + 1))
    
    echo "----------------------------------------"
    echo "Running test$num (frames=$frames, blocks=$blocks, nodiff=$nodiff)"
    echo "----------------------------------------"
    
    rm -rf mmu.sock mmu.pmem.img.*
    ./bin/mmu $frames $blocks &> test$num.mmu.out &
    MMU_PID=$!
    sleep 1
    
    # Verifica se MMU iniciou
    if ! kill -0 $MMU_PID 2>/dev/null; then
        echo "ERROR: MMU failed to start for test$num"
        FAILED=$((FAILED + 1))
        continue
    fi
    
    # Executa o teste com timeout de 60s
    timeout 60 ./bin/test$num &> test$num.out
    TEST_STATUS=$?
    
    kill -SIGINT $MMU_PID 2>/dev/null
    wait $MMU_PID 2>/dev/null
    rm -rf mmu.sock mmu.pmem.img.*
    
    # Verifica se o teste teve timeout
    if [ $TEST_STATUS -eq 124 ]; then
        echo "TIMEOUT: test$num exceeded 60 seconds"
        FAILED=$((FAILED + 1))
        continue
    fi
    
    # Test5 é especial: DEVE crashar (segfault esperado)
    if [ $num -eq 5 ]; then
        if [ $TEST_STATUS -ne 0 ]; then
            echo "PASSED: test$num (expected crash with segfault)"
            PASSED=$((PASSED + 1))
        else
            echo "FAILED: test$num (should have crashed but didn't)"
            FAILED=$((FAILED + 1))
        fi
        continue
    fi
    
    # Verifica se o teste crashou inesperadamente
    if [ $TEST_STATUS -ne 0 ] && [ $nodiff -eq 0 ]; then
        echo "CRASH: test$num exited with code $TEST_STATUS"
        FAILED=$((FAILED + 1))
        continue
    fi
    
    # Se nodiff=1, apenas verifica se executou sem crash
    if [ $nodiff -eq 1 ] ; then
        if [ $TEST_STATUS -eq 0 ]; then
            echo "PASSED: test$num (no diff check)"
            PASSED=$((PASSED + 1))
        else
            echo "FAILED: test$num (exit code $TEST_STATUS)"
            FAILED=$((FAILED + 1))
        fi
        continue
    fi
    
    # Compara as saídas
    DIFF_FAILED=0
    
    if ! diff mempager-tests/test$num.mmu.out test$num.mmu.out > /dev/null 2>&1 ; then
        echo "DIFF: test$num.mmu.out differs"
        DIFF_FAILED=1
    fi
    
    if ! diff mempager-tests/test$num.out test$num.out > /dev/null 2>&1 ; then
        echo "DIFF: test$num.out differs"
        DIFF_FAILED=1
    fi
    
    if [ $DIFF_FAILED -eq 0 ]; then
        echo "PASSED: test$num"
        PASSED=$((PASSED + 1))
    else
        echo "FAILED: test$num (output differs)"
        FAILED=$((FAILED + 1))
    fi
    
done < $TESTSPEC

echo ""
echo "=========================================="
echo "Resumo dos Testes"
echo "=========================================="
echo "Total:  $TOTAL"
echo "Passed: $PASSED"
echo "Failed: $FAILED"
echo "=========================================="

if [ $FAILED -eq 0 ]; then
    echo "✓ Todos os testes passaram!"
    exit 0
else
    echo "✗ Alguns testes falharam"
    exit 1
fi
