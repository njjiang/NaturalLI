#include "Trie.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sstream>
#ifdef HAVE_OPENMP
  #include <omp.h>
#endif

#include "Postgres.h"
#include "Utils.h"

#define MAP_SIZE 0x1 << 25



using namespace std;
using namespace btree;

//
// Trie::~Trie
//
Trie::~Trie() {
//  for (btree_map<word,Trie*>::iterator iter = children.begin(); iter != children.end(); ++iter) {
//    delete iter->second;
//  }
}

inline Trie* materializeSecond(const btree_map<word,trie_placeholder>::const_iterator& iter) {
  return (Trie*) (&(iter->second));
}

//
// Trie::add
//
void Trie::add(const edge* elements, const uint8_t& length,
               const Graph* graph) {
  // Corner cases
  if (length == 0) { return; }  // this case shouldn't actually happen normally...
  // Register child
  const word w = elements[0].source;
  assert (w > 0);
  const btree_map<word,trie_placeholder>::iterator childIter = children.find( w );
  Trie* child = NULL;
  if (childIter == children.end()) {
    // This is filthy. I'm so sorry...
    children[w]; // allocate space
    child = new(&(children[w])) Trie();
  } else {
    child = materializeSecond(childIter);
  }
  // Register information about child
  if (graph == NULL || graph->containsDeletion(elements[0])) {
    child->registerEdge(elements[0]);
  }
  // Recursive call
  if (length == 1) {
    child->data.isLeaf = true;    // Mark this as a leaf node
#if HIGH_MEMORY
    completions[w] = child;  // register a completion
#endif
  } else {
    child->add(&(elements[1]), length - 1, graph);
  }
}

inline uint8_t min(const uint8_t& a, const uint8_t b) { return a < b ? a : b; }

//
// Trie::addCompletion
//
inline void Trie::addCompletion(const Trie* child, const word& source,
                                edge* insertion, uint32_t& index) const {
  if (index < MAX_COMPLETIONS - 4) {
    // case: write directly to insertion buffer
    uint8_t numEdges = child->getEdges(&(insertion[index]));
    for (int i = 0; i < numEdges; ++i) { 
      insertion[index + i].source = source;
    }
    index += numEdges;
  } else {
    // case: write to temporary buffer and copy over
    edge buffer[4];
    uint8_t bufferedEdges = child->getEdges(buffer);
    uint8_t numEdges = min( MAX_COMPLETIONS - index, bufferedEdges );
    for (int i = 0; i < numEdges; ++i) { buffer[i].source = source; }
    memcpy(&(insertion[index]), buffer, numEdges * sizeof(edge));
    index += numEdges;
  }
}



//
// Trie::contains
//
const bool Trie::contains(const tagged_word* query, 
                          const uint8_t& queryLength,
                          const int16_t& mutationIndex,
                          edge* insertions,
                              uint32_t& mutableIndex) const {
  assert (queryLength > mutationIndex);

  // -- Part 1: Fill in completions --
  if (mutationIndex == -1) {
    const bool tooManyChildren = (children.size() > MAX_COMPLETIONS);
    if (!tooManyChildren) {
      // sub-case: add all children
      btree_map<word,trie_placeholder>::const_iterator iter;
      for (iter = children.begin(); iter != children.end(); ++iter) {
        addCompletion(materializeSecond(iter), iter->first, insertions, mutableIndex);
        if (mutableIndex >= MAX_COMPLETIONS) { break; }
      }
    } else {
#if HIGH_MEMORY
      // sub-case: too many children; only add completions
      for (btree_map<word,Trie*>::const_iterator iter = completions.begin(); iter != completions.end(); ++iter) {
        addCompletion(iter->second, iter->first, insertions, mutableIndex);
        if (mutableIndex >= MAX_COMPLETIONS) { break; }
      }
#endif
    }
  }
  
  // -- Part 2: Check containment --
  if (queryLength == 0) {
    // return whether the fact exists
    return isLeaf();
  } else {
    // Case: we're in the middle of the query
    btree_map<word,trie_placeholder>::const_iterator childIter = children.find( query[0].word );
    if (childIter == children.end()) {
      // Return false
      return false;
    } else {
      // Check the child
      return materializeSecond(childIter)
        ->contains(&(query[1]), 
                   queryLength - 1,
                   mutationIndex - 1,
                   insertions,
                   mutableIndex);
    }
  }
}

//
// Trie::memoryUsage
//
uint64_t Trie::memoryUsage(uint64_t* onFacts,
                           uint64_t* onStructure,
                           uint64_t* onCompletionCaching) const {
  // (me)
  (*onStructure) += sizeof(*this);
  // (completions)
#if HIGH_MEMORY
  (*onCompletionCaching) += (sizeof(word) + sizeof(Trie*)) * completions.size();
#endif
  // (children)
  for (btree_map<word,trie_placeholder>::const_iterator childIter = children.begin();
       childIter != children.end(); ++childIter) {
    (*onFacts) += sizeof(word);
    materializeSecond(childIter)->memoryUsage(onFacts, onStructure, onCompletionCaching);
  }
  // (return)
  return (*onFacts) + (*onStructure) + (*onCompletionCaching);
}

//
// ----------------------------------------------------------------------------
//

//
// TrieRoot::add()
//
void TrieRoot::add(const edge* elements, const uint8_t& length,
                   const Graph* graph) {
  // Add the fact
  Trie::add(elements, length, graph);
  // Register skip-gram
  const word w = elements[0].source;
  if (length > 1) {
    const word grandChildW = elements[1].source;
    assert (grandChildW > 0);
    skipGrams[grandChildW].push_back(w);
  }
}

//
// TrieRoot::contains
//
const bool TrieRoot::contains(const tagged_word* query, 
                              const uint8_t& queryLength,
                              const int16_t& mutationIndex,
                              edge* insertions) const {
  assert (queryLength > mutationIndex);
  uint32_t mutableIndex = 0;
  bool contains;
  if (mutationIndex == -1) {
    if (queryLength > 0) {
      btree_map<word,vector<word>>::const_iterator skipGramIter = skipGrams.find( query[0].word );
      if (skipGramIter != skipGrams.end()) {
        // Case: add anything that leads into the second term
        for (vector<word>::const_iterator iter = skipGramIter->second.begin(); iter != skipGramIter->second.end(); ++iter) {
          btree_map<word,trie_placeholder>::const_iterator childIter = children.find( *iter );
          if (childIter != children.end()) {
            addCompletion(materializeSecond(childIter), childIter->first, insertions, mutableIndex);
          }
          if (mutableIndex >= MAX_COMPLETIONS) { break; }
        }
      } else {
        // Case: we're kind of shit out of luck. We're inserting into the
        //       beginning of the sentence, but with no valid skip-grams.
        //       So, let's just add some starting words and pray.
        for (btree_map<word,trie_placeholder>::const_iterator childIter = children.begin(); childIter != children.end(); ++childIter) {
          addCompletion(materializeSecond(childIter), childIter->first, insertions, mutableIndex);
          if (mutableIndex >= MAX_COMPLETIONS) { break; }
        }
      }
    } else {
      // Case: add any single-term completions
      for (btree_map<word,trie_placeholder>::const_iterator iter = children.begin(); iter != children.end(); ++iter) {
        Trie* child = materializeSecond(iter);
        if (child->isLeaf()) {
          addCompletion(child, iter->first, insertions, mutableIndex);
          if (mutableIndex >= MAX_COMPLETIONS) { break; }
        }
      }
    }
    contains = Trie::contains(query, queryLength, -9000, insertions, mutableIndex);  // already added completions
  } else {
    contains = Trie::contains(query, queryLength, mutationIndex, insertions, mutableIndex);
  }

  // Return
  if (mutableIndex < MAX_COMPLETIONS) {
    insertions[mutableIndex].source = 0;
  }
  return contains;
}
  
//
// TrieRoot::memoryUsage()
//
uint64_t TrieRoot::memoryUsage(uint64_t* onFacts,
                               uint64_t* onStructure,
                               uint64_t* onCompletionCaching) const {
  // (make sure variables work)
  uint64_t a = 0;
  uint64_t b = 0;
  uint64_t c = 0;
  if (onFacts == NULL) {
    onFacts = &a;
  }
  if (onStructure == NULL) {
    onStructure = &b;
  }
  if (onCompletionCaching == NULL) {
    onCompletionCaching = &c;
  }
  Trie::memoryUsage(onFacts, onStructure, onCompletionCaching);
  // (skip-grams)
  for (btree_map<word,vector<word>>::const_iterator skipGramIter = skipGrams.begin();
       skipGramIter != skipGrams.end(); ++skipGramIter) {
    (*onCompletionCaching) += sizeof(word);
    (*onCompletionCaching) += sizeof(vector<word>) + sizeof(word) * skipGramIter->second.size();
  }
  return (*onFacts) + (*onStructure) + (*onCompletionCaching);
}

//
// ----------------------------------------------------------------------------
//

//
// LossyTrie::LossyTrie
//
LossyTrie::LossyTrie(const HashIntMap& counts
    ) : completions(counts) {
  // Compile completions graph
  // (collect statistics)
  uint64_t sum = counts.sum();
  uint32_t nonEmpty = counts.size();
  // (create variables)
  assert (sum < (0x1 << 31));  // make sure we won't overflow a uint32_t
  uint64_t size = 
      sum * sizeof(packed_insertion) +   // for the data
      sum * sizeof(uint8_t) +            // for the 'complete fact' indicator
      sizeof(uint8_t);                   // for the 'null pointer'
  if (sum > 1024) {
    printf("Allocating for %lu completions, over %u subfacts.\n",
           sum, nonEmpty);
    printf("  the data will use %lu MB memory.\n", size / 1000000);
  }
  this->completionData = (uint8_t*) malloc(size);
  memset(this->completionData, 0, size);

  // Partition completions data
  uint32_t completionDataPointer = 1;
  auto countToAddr = [&completionDataPointer](uint32_t count) -> uint32_t { 
    // (allocate space)
    uint32_t pointer = completionDataPointer;
    completionDataPointer = completionDataPointer
      + (count * sizeof(packed_insertion))  // data
      + sizeof(uint8_t);                    // flags
    return pointer + sizeof(uint8_t);
  };
  completions.mapValues(countToAddr);
}

//
// LossyTrie::~LossyTrie
//
LossyTrie::~LossyTrie() {
  free(completionData);
}

//
// LossyTrie::addCompletion
//
void LossyTrie::addCompletion(const uint32_t* fact, 
                              const uint32_t& factLength, 
                              const word& source,
                              const uint8_t& sourceSense,
                              const uint8_t& edgeType) {
  // Create the edge
  packed_insertion edge;
  edge.source = source;
  edge.sense = sourceSense;
  edge.type = edgeType;
  edge.endOfList = 1;
  // Compute the hashes
  uint32_t mainHash = fnv_32a_buf((uint8_t*) fact, factLength * sizeof(uint32_t),  FNV1_32_INIT);
  uint32_t auxHash = fnv_32a_buf((uint8_t*) fact, factLength * sizeof(uint32_t),  1154);
  // Do the lookup
  uint32_t pointer;
  if (!completions.get(mainHash, auxHash, &pointer)) {
    printf("No pointer was allocated for completion!\n");
    std::exit(1);
  }
  // Set the 'has completions' indicator'
  completionData[pointer - 1] |= 0x2;
  packed_insertion* insertions = (packed_insertion*) &(completionData[pointer]);
  if ( (completionData[pointer - 1] & 0x4) == 0) {  // check if bucket is full
    // Find a free spot
    uint16_t index = 0;
    if (insertions[index].source != 0) {
      while (!insertions[index].endOfList && index < MAX_COMPLETIONS) {
        index += 1;
      }
      insertions[index].endOfList = 0;
      index += 1;
    }
    // Set the insertion
    if (index < MAX_COMPLETIONS) {
      // Case: add this edge
      insertions[index] = edge;
    } else {
      // Case: this slot's full
      completionData[pointer - 1] |= 0x4;
    }
  }
}
  
//
// LossyTrie::addBeginInsertion
//
void LossyTrie::addBeginInsertion(const word& w0, 
                                  const uint8_t w0Sense,
                                  const uint8_t w0Type,
                                  const word& w1) {
  packed_insertion edge;
  edge.source = w0;
  edge.sense = w0Sense;
  edge.type = w0Type;
  edge.endOfList = 0;
  beginInsertions[w1].push_back(edge);
}

//
// LossyTrie::addFact
//
void LossyTrie::addFact(const uint32_t* fact, 
                        const uint32_t& factLength) {
  // Compute the hashes
  uint32_t mainHash = fnv_32a_buf((uint8_t*) fact, factLength * sizeof(uint32_t),  FNV1_32_INIT);
  uint32_t auxHash = fnv_32a_buf((uint8_t*) fact, factLength * sizeof(uint32_t),  1154);
  // Do the lookup
  uint32_t pointer;
  if (!completions.get(mainHash, auxHash, &pointer)) {
    printf("No pointer was allocated for completion!\n");
    std::exit(1);
  }
  // Set the 'is fact' indicator'
  completionData[pointer - 1] |= 0x1;
}
  
//
// LossyTrie::contains
//
const bool LossyTrie::contains(const tagged_word* taggedFact, 
                               const uint8_t& factLength,
                               const int16_t& mutationIndex,
                               edge* insertions) const {
  // Hash the fact
  word fact[factLength];
  for (uint32_t i = 0; i < factLength; ++i) {
    fact[i] = taggedFact[i].word;
  }
  uint32_t mainHash = fnv_32a_buf((uint8_t*) fact, factLength * sizeof(uint32_t),  FNV1_32_INIT);
  uint32_t auxHash = fnv_32a_buf((uint8_t*) fact, factLength * sizeof(uint32_t),  1154);

  // Look up the containment info
  bool contains = false;
  uint32_t pointer;
  if (completions.get(mainHash, auxHash, &pointer)) {
    contains = (completionData[pointer - 1] & 0x1) != 0;
  }

  // Look up completions
  if (mutationIndex >= 0) {
    // Case: regular completion
    mainHash = fnv_32a_buf((uint8_t*) fact, (mutationIndex + 1) * sizeof(uint32_t),  FNV1_32_INIT);
    auxHash = fnv_32a_buf((uint8_t*) fact, (mutationIndex + 1) * sizeof(uint32_t),  1154);
    if (completions.get(mainHash, auxHash, &pointer)) {
      // Check the 'has completions' indicator
      bool hasCompletions = (completionData[pointer - 1] & 0x2) != 0;
      uint16_t index = -1;
      // Populate completions
      if (hasCompletions) {
        packed_insertion* toRead = (packed_insertion*) &(completionData[pointer]);
        // Populate insertions
        do {
          index += 1;
          insertions[index].source = toRead[index].source;
          insertions[index].source_sense = toRead[index].sense;
          insertions[index].sink = 0;
          insertions[index].sink_sense = 0;
          insertions[index].type = toRead[index].type;
          insertions[index].cost = 1.0f;
        } while (index < MAX_COMPLETIONS && !toRead[index].endOfList);
      }
      // End insertions array
      index += 1;
      if (index < MAX_COMPLETIONS) {
        insertions[index].source = 0;
      }
    }

  } else if (factLength == 0) {
    // Case: degenerate
    insertions[0].source = 0;

  } else {
    // Case: prefix completion
    btree_map<word,vector<packed_insertion>>::const_iterator inserts
        = beginInsertions.find( taggedFact[0].word );
    if (inserts != beginInsertions.end()) {
      vector<packed_insertion> toRead = inserts->second;
      uint16_t numCompletions = toRead.size() < MAX_COMPLETIONS ? toRead.size() : MAX_COMPLETIONS;
      for (uint16_t index = 0; index < numCompletions; ++index) {
        insertions[index].source = toRead[index].source;
        insertions[index].source_sense = toRead[index].sense;
        insertions[index].sink = 0;
        insertions[index].sink_sense = 0;
        insertions[index].type = toRead[index].type;
        insertions[index].cost = 1.0f;
      }
      if (numCompletions < MAX_COMPLETIONS) {
        insertions[numCompletions].source = 0;
      }
    }
  }

  // Return
  return contains;
}

//
// ----------------------------------------------------------------------------
//

//
// ReadFactTrie
//
FactDB* ReadOldFactTrie(const uint64_t maxFactsToRead, const Graph* graph) {
  Trie* facts = new TrieRoot();
  char query[127];

  // Read valid deletions
  printf("Reading registered deletions...\n");
  btree_map<word,vector<edge>> word2senses;
  // (query)
  snprintf(query, 127, "SELECT DISTINCT (source) source, source_sense, type FROM %s WHERE source<>0 AND sink=0 ORDER BY type;", PG_TABLE_EDGE);
  PGIterator wordIter = PGIterator(query);
  uint32_t numValidInsertions = 0;
  while (wordIter.hasNext()) {
    // Get fact
    PGRow row = wordIter.next();
    // Create edge
    edge e;
    e.source       = fast_atoi(row[0]);
    assert (atoi(row[0]) == fast_atoi(row[0]));
    e.source_sense = fast_atoi(row[1]);
    assert (atoi(row[1]) == fast_atoi(row[1]));
    e.type  = fast_atoi(row[2]);
    assert (atoi(row[2]) == fast_atoi(row[2]));
    e.cost  = 1.0f;
    // Register edge
    word2senses[e.source].push_back(e);
    numValidInsertions += 1;
  }
  printf("  Done. %u words have sense tags\n", numValidInsertions);


  // Read facts
  printf("Reading facts...\n");
  // (query)
  if (maxFactsToRead == std::numeric_limits<uint64_t>::max()) {
    snprintf(query, 127,
             "SELECT gloss, weight FROM %s ORDER BY weight DESC;",
             PG_TABLE_FACT);
  } else {
    snprintf(query, 127,
             "SELECT gloss, weight FROM %s ORDER BY weight DESC LIMIT %lu;",
             PG_TABLE_FACT,
             maxFactsToRead);
  }
  PGIterator iter = PGIterator(query);
  uint64_t i = 0;
  edge buffer[256];
  uint8_t bufferLength;
  while (iter.hasNext()) {
    // Get fact
    PGRow row = iter.next();
    const char* gloss = row[0];
    uint32_t weight = fast_atoi(row[1]);
    assert (atoi(row[1]) == fast_atoi(row[1]));
    if (weight < MIN_FACT_COUNT) { break; }
    // Parse fact
    stringstream stream (&gloss[1]);
    string substr;
    bufferLength = 0;
    while( getline (stream, substr, ',' ) ) {
      // Parse the word
      word w = fast_atoi(substr.c_str());
      assert (atoi(substr.c_str()) == fast_atoi(substr.c_str()));
      // Register the word
      btree_map<word,vector<edge>>::iterator iter = word2senses.find( w );
      if (iter == word2senses.end() || iter->second.size() == 0) {
        buffer[bufferLength].source       = w;
        buffer[bufferLength].source_sense = 0;
        buffer[bufferLength].type         = 0;
        buffer[bufferLength].cost         = 1.0f;
      } else {
        buffer[bufferLength] = iter->second[0];
      }
      buffer[bufferLength].sink       = 0;
      buffer[bufferLength].sink_sense = 0;
      if (bufferLength >= MAX_FACT_LENGTH) { break; }
      bufferLength += 1;
    }
    // Error check
    if (cin.bad()) {
      printf("IO Error: %s\n", gloss);
      std::exit(2);
    }
    // Add fact
    // Add 'canonical' version
    facts->add(buffer, bufferLength, graph);
    // Add word sense variants
    for (uint32_t k = 0; k < bufferLength; ++k) {
      btree_map<word,vector<edge>>::iterator iter = word2senses.find( buffer[k].source );
      if (iter != word2senses.end() && iter->second.size() > 1) {
        for (uint32_t sense = 1; sense < iter->second.size(); ++sense) {
          buffer[k] = iter->second[sense];
          facts->add(buffer, bufferLength, graph);
        }
      }
    }
    // Debug
    i += 1;
    if (i % 1000000 == 0) {
      printf("  loaded %luM facts (%luMB memory used in Trie)\n",
             i / 1000000,
             facts->memoryUsage(NULL, NULL, NULL) / 1000000);
    }
  }

  // Return
  printf("  done reading the fact database (%lu facts read)\n", i);
  return facts;
}
  


/**
 * Return a map from a word, to the possible insertion types and word
 * senses of that word to be inserted.
 * This is represented as a vector of edges, where the source and
 * source sense are the relevant variables for the insertion.
 */
btree_map<word, vector<edge>> getWord2Senses() {
  // Read valid deletions
  printf("Reading registered deletions...\n");
  btree_map<word,vector<edge>> word2senses;

  // Query
  char query[128];
  snprintf(query, 127, "SELECT DISTINCT (source) source, source_sense, type FROM %s WHERE source<>0 AND sink=0 ORDER BY type;", PG_TABLE_EDGE);
  PGIterator wordIter = PGIterator(query);
  uint32_t numValidInsertions = 0;
  while (wordIter.hasNext()) {
    // Get fact
    PGRow row = wordIter.next();
    // Create edge
    edge e;
    e.source       = fast_atoi(row[0]);
    assert (atoi(row[0]) == fast_atoi(row[0]));
    e.source_sense = fast_atoi(row[1]);
    assert (atoi(row[1]) == fast_atoi(row[1]));
    e.type  = fast_atoi(row[2]);
    assert (atoi(row[2]) == fast_atoi(row[2]));
    e.cost  = 1.0f;
    // Register edge
    word2senses[e.source].push_back(e);
    numValidInsertions += 1;
  }
  printf("  Done. %u words have sense tags\n", numValidInsertions);

  // Return
  return word2senses;
}
  
/**
 * Apply the given function to every fact in the fact database.
 * This is a linear scan through the database.
 * The function takes as input a word array and a length, and returns
 * void as output. For example:
 *
 * <code>
 *   auto fn = [](word* fact, uint8_t factLength) -> void { }
 * </code>
 */
template<typename Functor> inline void foreachFact(Functor fn,
                                                   const uint64_t& maxFactsToRead) {
  // Construct the query
  char query[128];
  if (maxFactsToRead == std::numeric_limits<uint64_t>::max()) {
    snprintf(query, 127,
             "SELECT gloss, weight FROM %s ORDER BY weight DESC;",
             PG_TABLE_FACT);
  } else {
    snprintf(query, 127,
             "SELECT gloss, weight FROM %s ORDER BY weight DESC LIMIT %lu;",
             PG_TABLE_FACT,
             maxFactsToRead);
  }
  printf("  %s\n", query);
  PGIterator iter = PGIterator(query);
  word buffer[256];
  uint8_t bufferLength;
  uint64_t factsRead = 0;

  // Collect completion statistics
  uint32_t completionsLength = 0x1 << 10;
  uint32_t* completions = (uint32_t*) malloc(completionsLength * sizeof(uint16_t));
  memset(completions, 0, completionsLength * sizeof(uint16_t));
  while (iter.hasNext()) {
    // (query Postgres)
    PGRow row = iter.next();
    // (check minimum weight)
    uint32_t weight = fast_atoi(row[1]);
    if (weight >= MIN_FACT_COUNT) {
      // (define variables)
      const char* gloss = row[0];
      stringstream stream (&gloss[1]);
      string substr;
      bufferLength = 0;
      // (read fact gloss)
      while( getline (stream, substr, ',' ) ) {
        // (parse the word)
        word w = fast_atoi(substr.c_str());
        // (save the word)
        buffer[bufferLength] = w;
        bufferLength += 1;
      }
      // (hash the fact)
      uint32_t factHash = fnv_32a_buf(buffer, bufferLength * sizeof(uint32_t), FNV1_32_INIT);
      // (apply the function)
      fn(buffer, bufferLength);
    }
    // Debug
    if (factsRead % 1000 == 0) {
      printf("  iterated over %luk facts\n", factsRead / 1000);
    }
    factsRead += 1;
  }
}

/**
 * Get the number of completions for every partial fact in the
 * fact database. These are stored in the counts output map.
 */
void completionCounts(
    btree_map<word, vector<edge>>& word2sense,
    HashIntMap* counts,
    const uint64_t maxFactsToRead) {
  // Define function
  auto fn = [&word2sense,&counts](word* fact, uint8_t factLength) -> void { 
    for (uint8_t len = 1; len < factLength; ++len) {
      uint32_t mainHash = fnv_32a_buf((uint8_t*) fact, len * sizeof(uint32_t),  FNV1_32_INIT);
      uint32_t auxHash = fnv_32a_buf((uint8_t*) fact, len * sizeof(uint32_t),  1154);
      word nextWord = fact[len];
      counts->increment(mainHash, auxHash, word2sense[nextWord].size(), MAX_COMPLETIONS);
    }
    uint32_t mainHash = fnv_32a_buf((uint8_t*) fact, factLength * sizeof(uint32_t),  FNV1_32_INIT);
    uint32_t auxHash = fnv_32a_buf((uint8_t*) fact, factLength * sizeof(uint32_t),  1154);
    counts->increment(mainHash, auxHash, 0);
  };
  // Run function
  printf("Pass 1: collect statistics...\n");
  foreachFact( fn, maxFactsToRead );
  printf("  pass 1 done.\n");
}

/**
 * Add all the facts to the LossyTrie. Note that the Trie must have
 * been initialized with the completionCounts() function above.
 */
void addFacts(
    btree_map<word, vector<edge>>& word2sense,
    LossyTrie* trie,
    const uint64_t maxFactsToRead) {
  // Define function
  auto fn = [&word2sense,&trie](word* fact, uint8_t factLength) -> void { 
    // Add prefix completion
    if (factLength > 1) {
      auto iter = word2sense.find( fact[0] );
      if (iter != word2sense.end() && iter->second.size() > 1) {
        for (uint32_t sense = 0; sense < iter->second.size(); ++sense) {
          edge& insertion = iter->second[sense];
          trie->addBeginInsertion(fact[0], insertion.source_sense,
                                  insertion.type, fact[1]);
        }
      }
    }
    // Add completions
    for (uint8_t len = 1; len < factLength; ++len) {
      auto iter = word2sense.find( fact[len] );
      if (iter != word2sense.end() && iter->second.size() > 1) {
        for (uint32_t sense = 0; sense < iter->second.size(); ++sense) {
          edge& insertion = iter->second[sense];
          trie->addCompletion(fact, len,
                              insertion.source, insertion.source_sense,
                              insertion.type);
        }
      }
    }
    // Add complete fact
    trie->addFact(fact, factLength);
  };
  // Run function
  printf("Pass 2: Collect facts...\n");
  foreachFact( fn, maxFactsToRead );
  printf("  pass 2 done.\n");

}


/**
 * Read a LossyFactTrie from the database.
 */
FactDB* ReadFactTrie(const uint64_t& maxFactsToRead) {
  // Word senses
  btree_map<word, vector<edge>> word2sense = getWord2Senses();

  // Completion counts
  HashIntMap countsThenPointers(MAP_SIZE);
  completionCounts(word2sense, &countsThenPointers, maxFactsToRead);

  // Allocate Trie
  // ^^ countsThenPointers is still a map of counts ^^
  LossyTrie* trie = new LossyTrie(countsThenPointers);
  // vv countsThenPointers is now a map of pointers vv

  // Populate the data
  addFacts(word2sense, trie, maxFactsToRead);

  // Return
  return trie;
}
  
FactDB* ReadFactTrie() {
  return ReadFactTrie(std::numeric_limits<uint64_t>::max());
}
