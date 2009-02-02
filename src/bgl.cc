/* This file is (c) 2008-2009 Konstantin Isakov <ikm@users.sf.net>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#include "bgl.hh"
#include "btreeidx.hh"
#include "bgl_babylon.hh"
#include "file.hh"
#include "folding.hh"
#include "utf8.hh"
#include "chunkedstorage.hh"
#include <map>
#include <set>
#include <list>
#include <zlib.h>
#include <ctype.h>
#include <string.h>

namespace Bgl {

using std::map;
using std::multimap;
using std::set;
using std::wstring;
using std::list;
using std::pair;

using BtreeIndexing::WordArticleLink;
using BtreeIndexing::IndexedWords;

namespace
{
  enum
  {
    Signature = 0x584c4742, // BGLX on little-endian, XLGB on big-endian
    CurrentFormatVersion = 12 + BtreeIndexing::FormatVersion
  };

  struct IdxHeader
  {
    uint32_t signature; // First comes the signature, BGLX
    uint32_t formatVersion; // File format version, currently 1.
    uint32_t parserVersion; // Version of the parser used to parse the BGL file.
                            // If it's lower than the current one, the file is to
                            // be re-parsed.
    uint32_t foldingVersion; // Version of the folding algorithm used when building
                             // index. If it's different from the current one,
                             // the file is to be rebuilt.
    uint32_t articleCount; // Total number of articles, for informative purposes only
    uint32_t wordCount; // Total number of words, for informative purposes only
    /// Add more fields here, like name, description, author and such.
    uint32_t chunksOffset; // The offset to chunks' storage
    uint32_t indexOffset; // The offset of the index in the file.
    uint32_t resourceListOffset; // The offset of the list of resources
    uint32_t resourcesCount; // Number of resources stored
  } __attribute__((packed));

  bool indexIsOldOrBad( string const & indexFile )
  {
    File::Class idx( indexFile, "rb" );

    IdxHeader header;

    return idx.readRecords( &header, sizeof( header ), 1 ) != 1 ||
           header.signature != Signature ||
           header.formatVersion != CurrentFormatVersion ||
           header.parserVersion != Babylon::ParserVersion ||
           header.foldingVersion != Folding::Version;
  }

  // Removes the $1$-like postfix
  string removePostfix( string const & in )
  {
    if ( in.size() && in[ in.size() - 1 ] == '$' )
    {
      // Find the end of it and cut it, barring any unexpectedness
      for( long x = in.size() - 2; x >= 0; x-- )
      {
        if ( in[ x ] == '$' )
          return in.substr( 0, x );
        else
        if ( !isdigit( in[ x ] ) )
          break;
      }
    }

    return in;
  }

  // Since the standard isspace() is locale-specific, we need something
  // that would never mess up our utf8 input. The stock one worked fine under
  // Linux but was messing up strings under Windows.
  bool isspace_c( int c )
  {
    switch( c )
    {
      case ' ':
      case '\f':
      case '\n':
      case '\r':
      case '\t':
      case '\v':
        return true;

      default:
        return false;
    }
  }

  // Removes any leading or trailing whitespace
  void trimWs( string & word )
  {
    if ( word.size() )
    {
      unsigned begin = 0;

      while( begin < word.size() && isspace_c( word[ begin ] ) )
        ++begin;

      if ( begin == word.size() ) // Consists of ws entirely?
        word.clear();
      else
      {
        unsigned end = word.size();

        // Doesn't consist of ws entirely, so must end with just isspace()
        // condition.
        while( isspace_c( word[ end - 1 ] ) )
          --end;

        if ( end != word.size() || begin )
          word = string( word, begin, end - begin );
      }
    }
  }

  void addEntryToIndex( string & word,
                        uint32_t articleOffset,
                        IndexedWords & indexedWords,
                        vector< wchar_t > & wcharBuffer )
  {
    // Strip any leading or trailing whitespaces
    trimWs( word );

    // Check the input word for a superscript postfix ($1$, $2$ etc), which
    // signifies different meaning in Bgl files. We emit different meaning
    // as different articles, but they appear in the index as the same word.

    if ( word.size() && word[ word.size() - 1 ] == '$' )
    {
      word = removePostfix( word );
      trimWs( word );
    }

    // Convert the word from utf8 to wide chars

    if ( wcharBuffer.size() <= word.size() )
      wcharBuffer.resize( word.size() + 1 );

    long result = Utf8::decode( word.c_str(), word.size(),
                                &wcharBuffer.front() );

    if ( result < 0 )
    {
      fprintf( stderr, "Failed to decode utf8 of headword %s, skipping it.\n",
               word.c_str() );
      return;
    }

    wcharBuffer[ result ] = 0;

    // Now make its folded version

    wstring folded = Folding::apply( &wcharBuffer.front() );

    /// Try to conserve the memory usage of the string
    folded.reserve( folded.size() );

    // Insert new entry into an index

    IndexedWords::iterator i = indexedWords.insert(
      IndexedWords::value_type( folded, vector< WordArticleLink >() ) ).first;

    // Try to conserve memory somewhat -- slow insertions are ok
    i->second.reserve( i->second.size() + 1 );

    i->second.push_back( WordArticleLink( word, articleOffset ) );
  }


  DEF_EX( exFailedToDecompressArticle, "Failed to decompress article's body", Dictionary::Ex )
  DEF_EX( exChunkIndexOutOfRange, "Chunk index is out of range", Dictionary::Ex )

  class BglDictionary: public BtreeIndexing::BtreeDictionary
  {
    File::Class idx;
    IdxHeader idxHeader;
    string dictionaryName;
    ChunkedStorage::Reader chunks;

  public:

    BglDictionary( string const & id, string const & indexFile,
                   string const & dictionaryFile );

    virtual string getName() throw()
    { return dictionaryName; }

    virtual map< Dictionary::Property, string > getProperties() throw()
    { return map< Dictionary::Property, string >(); }

    virtual unsigned long getArticleCount() throw()
    { return idxHeader.articleCount; }

    virtual unsigned long getWordCount() throw()
    { return idxHeader.wordCount; }

    virtual vector< wstring > findHeadwordsForSynonym( wstring const & )
      throw( std::exception );

    virtual string getArticle( wstring const &, vector< wstring > const & alts )
      throw( Dictionary::exNoSuchWord, std::exception );

    virtual void getResource( string const & name,
                              vector< char > & data ) throw( Dictionary::exNoSuchResource,
                                                             std::exception );

  private:


    /// Loads an article with the given offset, filling the given strings.
    void loadArticle( uint32_t offset, string & headword,
                      string & displayedHeadword, string & articleText );

    void replaceCharsetEntities( string & );
  };

  BglDictionary::BglDictionary( string const & id, string const & indexFile,
                                string const & dictionaryFile ):
    BtreeDictionary( id, vector< string >( 1, dictionaryFile ) ),
    idx( indexFile, "rb" ),
    idxHeader( idx.read< IdxHeader >() ),
    chunks( idx, idxHeader.chunksOffset )
  {
    idx.seek( sizeof( idxHeader ) );

    // Read the dictionary's name

    size_t len = idx.read< uint32_t >();

    vector< char > nameBuf( len );

    idx.read( &nameBuf.front(), len );

    dictionaryName = string( &nameBuf.front(), len );

    // Initialize the index

    idx.seek( idxHeader.indexOffset );

    openIndex( idx );
  }


  void BglDictionary::loadArticle( uint32_t offset, string & headword,
                                   string & displayedHeadword,
                                   string & articleText )
  {
    vector< char > chunk;

    char * articleData = chunks.getBlock( offset, chunk );

    headword = articleData;

    displayedHeadword = articleData + headword.size() + 1;

    articleText =
      string( articleData + headword.size() + 
                displayedHeadword.size() + 2 );
  }

  vector< wstring > BglDictionary::findHeadwordsForSynonym( wstring const & str )
    throw( std::exception )
  {
    vector< wstring > result;

    vector< WordArticleLink > chain = findArticles( str );

    wstring caseFolded = Folding::applySimpleCaseOnly( str );

    for( unsigned x = 0; x < chain.size(); ++x )
    {
      string headword, displayedHeadword, articleText;

      loadArticle( chain[ x ].articleOffset,
                   headword, displayedHeadword, articleText );

      wstring headwordDecoded = Utf8::decode( removePostfix(  headword ) );

      if ( caseFolded != Folding::applySimpleCaseOnly( headwordDecoded ) )
      {
        // The headword seems to differ from the input word, which makes the
        // input word its synonym.
        result.push_back( headwordDecoded );
      }
    }

    return result;
  }

  // Converts a $1$-like postfix to a <sup>1</sup> one
  string postfixToSuperscript( string const & in )
  {
    if ( !in.size() || in[ in.size() - 1 ] != '$' )
      return in;

    for( long x = in.size() - 2; x >= 0; x-- )
    {
      if ( in[ x ] == '$' )
      {
        if ( in.size() - x - 2 > 2 )
        {
          // Large postfixes seem like something we wouldn't want to show --
          // some dictionaries seem to have each word numbered using the
          // postfix.
          return in.substr( 0, x );
        }
        else
          return in.substr( 0, x ) + "<sup>" + in.substr( x + 1, in.size() - x - 2 ) + "</sup>";
      }
      else
      if ( !isdigit( in[ x ] ) )
        break;
    }

    return in;
  }

  string BglDictionary::getArticle( wstring const & word,
                                    vector< wstring > const & alts )
    throw( Dictionary::exNoSuchWord, std::exception )
  {
    vector< WordArticleLink > chain = findArticles( word );

    for( unsigned x = 0; x < alts.size(); ++x )
    {
      /// Make an additional query for each alt

      vector< WordArticleLink > altChain = findArticles( alts[ x ] );

      chain.insert( chain.end(), altChain.begin(), altChain.end() );
    }

    multimap< wstring, pair< string, string > > mainArticles, alternateArticles;

    set< uint32_t > articlesIncluded; // Some synonims make it that the articles
                                      // appear several times. We combat this
                                      // by only allowing them to appear once.

    wstring wordCaseFolded = Folding::applySimpleCaseOnly( word );

    for( unsigned x = 0; x < chain.size(); ++x )
    {
      if ( articlesIncluded.find( chain[ x ].articleOffset ) != articlesIncluded.end() )
        continue; // We already have this article in the body.

      // Now grab that article

      string headword, displayedHeadword, articleText;

      loadArticle( chain[ x ].articleOffset,
                   headword, displayedHeadword, articleText );

      // Ok. Now, does it go to main articles, or to alternate ones? We list
      // main ones first, and alternates after.

      // We do the case-folded and postfix-less comparison here.

      wstring headwordStripped =
        Folding::applySimpleCaseOnly( Utf8::decode( removePostfix( headword ) ) );

      multimap< wstring, pair< string, string > > & mapToUse = 
        ( wordCaseFolded == headwordStripped ) ?
          mainArticles : alternateArticles;

      mapToUse.insert( pair< wstring, pair< string, string > >(
        Folding::applySimpleCaseOnly( Utf8::decode( headword ) ),
        pair< string, string >( 
          displayedHeadword.size() ? displayedHeadword : headword,
          articleText ) ) );

      articlesIncluded.insert( chain[ x ].articleOffset );
    }

    if ( mainArticles.empty() && alternateArticles.empty() )
      throw Dictionary::exNoSuchWord();

    string result;

    multimap< wstring, pair< string, string > >::const_iterator i;

    string cleaner = "</font>""</font>""</font>""</font>""</font>""</font>"
                     "</font>""</font>""</font>""</font>""</font>""</font>"
                     "</b></b></b></b></b></b></b></b>"
                     "</i></i></i></i></i></i></i></i>";

    for( i = mainArticles.begin(); i != mainArticles.end(); ++i )
    {
        result += "<h3>";
        result += postfixToSuperscript( i->second.first );
        result += "</h3>";
        result += i->second.second;
        result += cleaner;
    }

    for( i = alternateArticles.begin(); i != alternateArticles.end(); ++i )
    {
        result += "<h3>";
        result += postfixToSuperscript( i->second.first );
        result += "</h3>";
        result += i->second.second;
        result += cleaner;
    }
    // Do some cleanups in the text

    replaceCharsetEntities( result );

    return result;
  }

  void BglDictionary::getResource( string const & name,
                                   vector< char > & data )
    throw( Dictionary::exNoSuchResource, std::exception )
  {
    string nameLowercased = name;

    for( string::iterator i = nameLowercased.begin(); i != nameLowercased.end();
         ++i )
      *i = tolower( *i );

    idx.seek( idxHeader.resourceListOffset );

    for( size_t count = idxHeader.resourcesCount; count--; )
    {
      vector< char > nameData( idx.read< uint32_t >() );
      idx.read( &nameData.front(), nameData.size() );

      for( size_t x = nameData.size(); x--; )
        nameData[ x ] = tolower( nameData[ x ] );

      uint32_t offset = idx.read< uint32_t >();

      if ( string( &nameData.front(), nameData.size() ) == nameLowercased )
      {
        // We have a match.

        idx.seek( offset );

        data.resize( idx.read< uint32_t >() );

        vector< unsigned char > compressedData( idx.read< uint32_t >() );

        idx.read( &compressedData.front(), compressedData.size() );

        unsigned long decompressedLength = data.size();

        if ( uncompress( (unsigned char *)&data.front(),
                         &decompressedLength,
                         &compressedData.front(),
                         compressedData.size() ) != Z_OK ||
             decompressedLength != data.size() )
        {
          printf( "Failed to decompress resource %s, ignoring it.\n",
            name.c_str() );
          throw Dictionary::exNoSuchResource();
        }

        return;
      }
    }

    throw Dictionary::exNoSuchResource();    
  }

  /// Replaces <CHARSET c="t">1234;</CHARSET> occurences with &#x1234;
  void BglDictionary::replaceCharsetEntities( string & text )
  {
    string lowercased = text;

    // Make a lowercased version of text, used for searching only. Only touch
    // symbols < 0x80 to avoid any weird results.
    for( unsigned x = lowercased.size(); x--; )
      if ( (unsigned char )lowercased[ x ] < 0x80 )
        lowercased[ x ] = tolower( lowercased[ x ] );

    size_t prevPos = 0;

    for( ;; )
    {
      size_t pos = lowercased.find( "<charset c=\"t\">", prevPos );

      if ( pos == string::npos )
        break;

      if ( lowercased.size() - pos < 30 )
      {
        // This is not right, the string is too short, leave it alone
        break;
      }

      prevPos = pos + 1;

      if ( lowercased.substr( pos + 15 + 4, 11 ) != ";</charset>" )
      {
        // The ending doesn't match
        printf( "!!!!!!ending mismatch\n" );
        continue;
      }

      // Check if digits are all hex

      if ( !isxdigit( lowercased[ pos + 15 ] ) ||
           !isxdigit( lowercased[ pos + 16 ] ) ||
           !isxdigit( lowercased[ pos + 17 ] ) ||
           !isxdigit( lowercased[ pos + 18 ] ) )
      {
        printf( "!!!!!!!!not hex digits\n" );
        continue;
      }

      // Ok, replace now.

      lowercased.replace( pos, 15, "&#x" );
      lowercased.erase( pos + 8, 10 );

      text.replace( pos, 15, "&#x" );
      text.erase( pos + 8, 10 );
    }

    prevPos = 0;

    // Copy-pasted version for <charset c=t>. This should all be replaced
    // by regexps.
    for( ;; )
    {
      size_t pos = lowercased.find( "<charset c=t>", prevPos );

      if ( pos == string::npos )
        break;

      if ( lowercased.size() - pos < 28 )
      {
        // This is not right, the string is too short, leave it alone
        break;
      }

      prevPos = pos + 1;

      if ( lowercased.substr( pos + 13 + 4, 11 ) != ";</charset>" )
      {
        // The ending doesn't match
        printf( "!!!!!!ending mismatch\n" );
        continue;
      }

      // Check if digits are all hex

      if ( !isxdigit( lowercased[ pos + 13 ] ) ||
           !isxdigit( lowercased[ pos + 14 ] ) ||
           !isxdigit( lowercased[ pos + 15 ] ) ||
           !isxdigit( lowercased[ pos + 16 ] ) )
      {
        printf( "!!!!!!!!not hex digits\n" );
        continue;
      }

      // Ok, replace now.

      lowercased.replace( pos, 13, "&#x" );
      lowercased.erase( pos + 8, 10 );

      text.replace( pos, 13, "&#x" );
      text.erase( pos + 8, 10 );
    }
  }

  class ResourceHandler: public Babylon::ResourceHandler
  {
    File::Class & idxFile;
    list< pair< string, uint32_t > > resources;

  public:

    ResourceHandler( File::Class & idxFile_ ): idxFile( idxFile_ )
    {}

    list< pair< string, uint32_t > > const & getResources() const
    { return resources; }

  protected:
    virtual void handleBabylonResource( string const & filename,
                                        char const * data, size_t size );
  };

  void ResourceHandler::handleBabylonResource( string const & filename,
                                               char const * data, size_t size )
  {
    //printf( "Handling resource file %s (%u bytes)\n", filename.c_str(), size );

    vector< unsigned char > compressedData( compressBound( size ) );

    unsigned long compressedSize = compressedData.size();

    if ( compress( &compressedData.front(), &compressedSize,
                   (unsigned char const *) data, size ) != Z_OK )
    {
      fprintf( stderr, "Failed to compress the body of resource %s, dropping it.\n",
               filename.c_str() );
      return;
    }

    resources.push_back( pair< string, uint32_t >( filename, idxFile.tell() ) );

    idxFile.write< uint32_t >( size );
    idxFile.write< uint32_t >( compressedSize );
    idxFile.write( &compressedData.front(), compressedSize );
  }
}



vector< sptr< Dictionary::Class > > Format::makeDictionaries(
                                            vector< string > const & fileNames,
                                            string const & indicesDir,
                                            Dictionary::Initializing & initializing )
  throw( std::exception )
{
  vector< sptr< Dictionary::Class > > dictionaries;

  for( vector< string >::const_iterator i = fileNames.begin(); i != fileNames.end();
       ++i )
  {
    // Skip files with the extensions different to .bgl to speed up the
    // scanning
    if ( i->size() < 4 ||
        strcasecmp( i->c_str() + ( i->size() - 4 ), ".bgl" ) != 0 )
      continue;

    Babylon b( *i );

    if ( !b.open() )
      continue;

    // Got the file -- check if we need to rebuid the index

    vector< string > dictFiles( 1, *i );

    string dictId = makeDictionaryId( dictFiles );

    string indexFile = indicesDir + dictId;

    if ( needToRebuildIndex( dictFiles, indexFile ) || indexIsOldOrBad( indexFile ) )
    {
      // Building the index

      std::string sourceCharset, targetCharset;

      if ( !b.read( sourceCharset, targetCharset ) )
      {
        fprintf( stderr, "Failed to start reading from %s, skipping it\n", i->c_str() );
        continue;
      }

      initializing.indexingDictionary( b.title() );

      File::Class idx( indexFile, "wb" );

      IdxHeader idxHeader;

      memset( &idxHeader, 0, sizeof( idxHeader ) );

      // We write a dummy header first. At the end of the process the header
      // will be rewritten with the right values.

      idx.write( idxHeader );

      idx.write< uint32_t >( b.title().size() );
      idx.write( b.title().data(), b.title().size() );

      // This is our index data that we accumulate during the loading process.
      // For each new word encountered, we emit the article's body to the file
      // immediately, inserting the word itself and its offset in this map.
      // This map maps folded words to the original words and the corresponding
      // articles' offsets.
      IndexedWords indexedWords;

      // We use this buffer to decode utf8 into it.
      vector< wchar_t > wcharBuffer;

      ChunkedStorage::Writer chunks( idx );

      uint32_t articleCount = 0, wordCount = 0;

      ResourceHandler resourceHandler( idx );

      b.setResourcePrefix( string( "bres://" ) + dictId + "/" );

      for( ; ; )
      {
        bgl_entry e = b.readEntry( &resourceHandler );

        if ( e.headword.empty() )
          break;

        // Save the article's body itself first

        uint32_t articleAddress = chunks.startNewBlock();

        chunks.addToBlock( e.headword.c_str(), e.headword.size() + 1 );
        chunks.addToBlock( e.displayedHeadword.c_str(), e.displayedHeadword.size() + 1 );
        chunks.addToBlock( e.definition.c_str(), e.definition.size() + 1 );

        // Add entries to the index

        addEntryToIndex( e.headword, articleAddress, indexedWords, wcharBuffer );

        for( unsigned x = 0; x < e.alternates.size(); ++x )
          addEntryToIndex( e.alternates[ x ], articleAddress, indexedWords, wcharBuffer );

        wordCount += 1 + e.alternates.size();
        ++articleCount;
      }

      // Finish with the chunks

      idxHeader.chunksOffset = chunks.finish();

      printf( "Writing index...\n" );

      // Good. Now build the index

      idxHeader.indexOffset = BtreeIndexing::buildIndex( indexedWords, idx );

      // Save the resource's list.

      idxHeader.resourceListOffset = idx.tell();
      idxHeader.resourcesCount = resourceHandler.getResources().size();

      for( list< pair< string, uint32_t > >::const_iterator i =
          resourceHandler.getResources().begin();
           i != resourceHandler.getResources().end(); ++i )
      {
        idx.write< uint32_t >( i->first.size() );
        idx.write( i->first.data(), i->first.size() );
        idx.write< uint32_t >( i->second );
      }

      // That concludes it. Update the header.

      idxHeader.signature = Signature;
      idxHeader.formatVersion = CurrentFormatVersion;
      idxHeader.parserVersion = Babylon::ParserVersion;
      idxHeader.foldingVersion = Folding::Version;
      idxHeader.articleCount = articleCount;
      idxHeader.wordCount = wordCount;

      idx.rewind();

      idx.write( &idxHeader, sizeof( idxHeader ) );
    }

    dictionaries.push_back( new BglDictionary( dictId,
                                               indexFile,
                                               *i ) );
  }

  return dictionaries;
}

}
